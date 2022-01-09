//
//	Compiled with
//	
//	g++ main.cpp -std=c++17 -lsensors -lasound -o topbar
//

#include <iostream>
#include <iomanip>
#include <queue>
#include <fstream>
#include <sstream>
#include <numeric>
#include <thread>
#include <chrono>
#include <map>
#include <vector>
#include <string>
#include <filesystem>


// For temperature measurements purpose
#include <sensors/sensors.h>

#include <alsa/asoundlib.h>

// Base log function implementation with simple parser
void log(const char* s)
{
	while (*s)
	{
		if (*s == '%')
			++s;
		std::cout << *s++;
	}
	std::cout << std::endl;
}

template<typename T, typename... Args>
void log(const char* s, T value, Args... args)
{
	while (*s)
	{
		if (*s == '%')
		{
			++s;
			if (!(*(s) == '%'))
			{
				std::cout << value;
				log(s, args...); return;
			}
		}
		std::cout << *s++;
	}
}

//
//	Macros
//

#define DEBUG_LOGS

#ifdef DEBUG_LOGS
	#define CLRSCR() log("\e[1;1H\e[2J")
	#define HIGH_TEXT(msg, ...) log("\033[36m" msg "\033[00m\n", ##__VA_ARGS__)
	#define LOG_INFO(msg, ...) log("\033[32m[ % ]: In Line % of file %\n- " msg "\033[00m\n", __TIME__, __LINE__, __FILE__, ##__VA_ARGS__)
	#define LOG_WARN(msg, ...) log("\033[33m[ % ]: In Line % of file %\n- " msg "\033[00m\n", __TIME__, __LINE__, __FILE__, ##__VA_ARGS__)
	#define LOG_ERROR(msg, ...) log("\033[31m[ % ]: In Line % of file %\n- " msg "\033[00m\n", __TIME__, __LINE__, __FILE__, ##__VA_ARGS__)
#else
	#define CLRSCR()
	#define HIGH_TEXT(msg, ...)
	#define LOG_INFO(msg, ...)
	#define LOG_WARN(msg, ...)
	#define LOG_ERROR(msg, ...)
#endif

//
//	Globals
//

bool app_is_running = true;

//
//	CPU Metrics
//

namespace cpu
{
	// For moving average of cpu metrics
	const size_t SAMPLES = 10;
	std::vector<float> metrics_queue;

	// First overload to get real status from file
	float get_cpu_metrics()
	{
		// For delta calc purpose
		static size_t previous_idle_time = 0, previous_total_time = 0;

		// RAW cpu metrics
		std::vector<size_t> cpu_times;

		// File open scope
		{
			// Open cpu status file
			std::ifstream proc_stat("/proc/stat");

			// Skip "CPU  " string in first line
			proc_stat.ignore(5, ' ');

			// Store results in cpu_times vector as size_t
			for (size_t time; proc_stat >> time; cpu_times.push_back(time));
		}

		if (cpu_times.size() < 4)
			return -1.0f;

		// Third metric is idle time of cpu
		const size_t idle_time = cpu_times[3];

		// Perform the sum of all elements in vector
		const size_t total_time = std::accumulate(cpu_times.begin(), cpu_times.end(), 0);

		// Perform delta times calc
		const float idle_time_delta = idle_time - previous_idle_time;
		const float total_time_delta = total_time - previous_total_time;

		// Update static states of function
		previous_idle_time = idle_time;
		previous_total_time = total_time;

		// Update metrics queue using certain amount of samples
		if (metrics_queue.size() >= SAMPLES)
			metrics_queue.erase(metrics_queue.begin());

		metrics_queue.push_back(float{ 100.0f * (1.0f - idle_time_delta / total_time_delta) });

		// Get cpu utilization in percent
		return float { 
			std::accumulate(metrics_queue.begin(), metrics_queue.end(), 0) / float(metrics_queue.size()) 
		};
	}
}

//
//	GPU Metrics
//

//
//	RAM Metrics
//

namespace ram
{
	struct status
	{
		float used;
		float total;
		float percent;
	};

	// Constants
	const size_t METRICS_NUMBER = 53;
	const float KB_TO_GB = 1.0f / 1048576.0f;

	// For moving average of RAM metrics
	const size_t SAMPLES = 5;
	std::vector<float> metrics_queue;

	// First overload to get real status from file
	status get_ram_metrics()
	{
		std::vector<float> ram_metrics;
		ram_metrics.reserve(METRICS_NUMBER);

		// File open scope
		{
			// Open memory status file
			std::ifstream mem_info("/proc/meminfo");

			// Lambda to process values from file
			auto process_line = [&mem_info](float& metric) -> bool {
				mem_info.ignore(256, ':');
				return mem_info >> metric ? true : false;
			};

			// Extract all metrics from meminfo
			for (float metric; process_line(metric); ram_metrics.push_back(metric));
		}

		// Position of MemTotal
		const float mem_total = ram_metrics[0];
		// Position of MemFree
		const float mem_free = ram_metrics[1];
		// Position of Buffers
		const float mem_buffer = ram_metrics[3];
		// Position of Cached:
		const float mem_cached = ram_metrics[4];
		// Position of SReclaimable
		const float mem_sreclaimable = ram_metrics[23];

		// Calculates used memory of system
		const float mem_used = mem_total - mem_free - mem_buffer - mem_cached - mem_sreclaimable;
		
		// Update metrics queue using certain amount of samples
		if (metrics_queue.size() >= SAMPLES)
			metrics_queue.erase(metrics_queue.begin());

		metrics_queue.push_back(mem_used);

		const float mem_used_avg = std::accumulate(metrics_queue.begin(), metrics_queue.end(), 0) / float(metrics_queue.size());
		const float mem_used_percent_avg = (mem_used_avg / mem_total) * 100.0f;

		// Get ram status
		return status { 
			mem_used_avg * KB_TO_GB,
			mem_total * KB_TO_GB,
			mem_used_percent_avg
		};
	}
}

//
//	TEMP Metrics
//
namespace temp
{
	// Temperature chip data
	const sensors_chip_name* cpu_chip_name = nullptr;
	const sensors_subfeature* cpu_sub_feature = nullptr;

	// Temperature sensors types
	const std::string sensor_types = "coretemp via_cputemp cpu_thermal k10temp zenpower acpitz";

	void init_sensors()
	{
		if (!sensors_init(NULL))
			LOG_INFO("Successful initialized libsensor");
		else
			LOG_ERROR("Failed to initialize libsensor");

		int nr = 0;
		for (const sensors_chip_name* chip = sensors_get_detected_chips(NULL, &nr); chip; chip = sensors_get_detected_chips(0, &nr))
		{
			if (sensor_types.find(std::string(chip->prefix)) != std::string::npos)
			{
				LOG_INFO("Found temperature sensor, prefix: %, path: %", chip->prefix, chip->path);
				int ft = 0;
				for (const sensors_feature* feature = sensors_get_features(chip, &ft); feature; sensors_get_features(chip, &ft))
				{
					if (std::string(feature->name).find("temp1") != std::string::npos)
					{
						LOG_INFO("Found temperature sensor feature: %", feature->name);

						cpu_chip_name = chip;
						cpu_sub_feature = sensors_get_subfeature(chip, feature, SENSORS_SUBFEATURE_TEMP_INPUT);

						LOG_INFO("Selected sensor subfeature: % / type: %", cpu_sub_feature->name, cpu_sub_feature->type);
						break;
					}
				}
				break;
			}
		}
	}

	// For moving average of TEMP metrics
	const size_t SAMPLES = 5;
	std::vector<float> metrics_queue;

	float get_cpu_temperature_metrics()
	{
		double temperature;
		sensors_get_value(cpu_chip_name, cpu_sub_feature->number, &temperature);

		// Update metrics queue using certain amount of samples
		if (metrics_queue.size() >= SAMPLES)
			metrics_queue.erase(metrics_queue.begin());

		metrics_queue.push_back(temperature);
	
		return std::accumulate(metrics_queue.begin(), metrics_queue.end(), 0) / float(metrics_queue.size());
	}
}

//
//	BATTERY Metrics
//
namespace battery
{
	struct status
	{
		int capacity;
		bool charging;
	};

	// Constants
	const char* POWER_SUPPLIES_DIR = "/sys/class/power_supply/";
	const char* BATTERY_PREFIX = "BAT";
	
	// Supplies path
	const std::filesystem::path power_supplies{POWER_SUPPLIES_DIR};

	// Supplies storages
	std::map<int, std::pair<std::string, std::string>> batteries;

	bool has_battery()
	{
		return !std::filesystem::is_empty(power_supplies);
	}

	void check_supplies()
	{
		for (const auto& entry : std::filesystem::directory_iterator(power_supplies))
		{
			std::string entry_path = entry.path().string();

			size_t bat_search = entry_path.find(BATTERY_PREFIX);

			if (bat_search != std::string::npos)
			{
				// Generates stream for value extraction
				std::stringstream ss(entry_path);
				ss.ignore(bat_search, '\0');

				// Get number of battery
				int index;
				ss >> index;

				ss.clear();
				ss.str(std::string());
				ss << entry_path << "/capacity";
				std::string capacity_path = ss.str();

				ss.clear();
				ss.str(std::string());
				ss << entry_path << "/status";
				std::string status_path = ss.str();

				batteries[index] = { capacity_path, status_path };
			}
		}
	}

	status get_battery_metrics()
	{
		std::ifstream capacity_file(batteries.at(0).first);
		std::ifstream status_file (batteries.at(0).second);

		int battery_value;
		capacity_file >> battery_value;

		std::string battery_status;
		status_file >> battery_status;

		return status {
			battery_value,
			(battery_status != "Discharging")
		};
	}
}

//
//	POWER Metrics
//

//
//	VOL Metrics
//
namespace audio
{
	// Volume configs
	const char* volume_card = "default";
	const char* volume_mixer_name = "Master";
	const int volume_mixer_index = 0;

	snd_mixer_t* volume_handle;
	snd_mixer_elem_t* volume_element;
	snd_mixer_selem_id_t* volume_sid;

	// Mic configs
	const char* mic_card = "default";
	const char* mic_mixer_name = "Capture";
	const int mic_mixer_index = 0;

	snd_mixer_t* mic_handle;
	snd_mixer_elem_t* mic_element;
	snd_mixer_selem_id_t* mic_sid;

	void init_connection(
		const char* card, const char* mixer_name, const int mixer_index,
		snd_mixer_t** handle, snd_mixer_elem_t** element, snd_mixer_selem_id_t** sid
	)
	{
		snd_mixer_selem_id_alloca(&(*sid));

		snd_mixer_selem_id_set_name(*sid, mixer_name);
		snd_mixer_selem_id_set_index(*sid, mixer_index);

		if (snd_mixer_open(&(*handle), 0) < 0)
			LOG_ERROR("Failed to open sound mixer");
		else
			LOG_INFO("Sound mixer successfully opened");
			

		if (snd_mixer_attach(*handle, card) < 0)
			LOG_ERROR("Failed to attach sound mixer");
		else
			LOG_INFO("Sound mixer successfully attached");

		if (snd_mixer_selem_register(*handle, NULL, NULL) < 0)
			LOG_ERROR("Failed to register sound element");
		else
			LOG_INFO("Sound element successfully registered");

		const int mixer = snd_mixer_load(*handle);
		if (mixer < 0)
			LOG_ERROR("Failed to load mixer");
		else
			LOG_INFO("Mixer successfully loaded");

		*element = snd_mixer_find_selem(*handle, *sid);
		if (!*element)
			LOG_ERROR("Failed to find sound element");
		else
			LOG_INFO("Sound element successfully found");
	}

	void init_mic_connections()
	{
		init_connection(mic_card, mic_mixer_name, mic_mixer_index, &mic_handle, &mic_element, &mic_sid);
	}

	void init_volume_connections()
	{
		init_connection(volume_card, volume_mixer_name, volume_mixer_index, &volume_handle, &volume_element, &volume_sid);
	}

	void close_mic_connection()
	{
		snd_mixer_close(volume_handle);
	}

	void close_volume_connection()
	{
		snd_mixer_close(mic_handle);
	}

	long get_mic()
	{
		snd_mixer_handle_events(mic_handle);

		long min_volume, max_volume;
		long out_volume_left, out_volume_right;

		snd_mixer_selem_get_capture_volume_range(mic_element, &min_volume, &max_volume);
		
		if (snd_mixer_selem_get_capture_volume(mic_element, SND_MIXER_SCHN_FRONT_LEFT, &out_volume_left) < 0)
		{
			snd_mixer_close(mic_handle);
			LOG_ERROR("Failed to get volume of sound element in left chanel");
		}

		if (snd_mixer_selem_get_capture_volume(mic_element, SND_MIXER_SCHN_FRONT_RIGHT, &out_volume_right) < 0)
		{
			snd_mixer_close(mic_handle);
			LOG_ERROR("Failed to get volume of sound element in rigth chanel");
		}

		// Calculate real maximum volume
		max_volume -= min_volume;

		// Adjust to minimum bound
		out_volume_left -= min_volume;
		out_volume_right -= min_volume;

		// Adjust to 100% scale
		out_volume_left = 100 * (out_volume_left) / max_volume;
		out_volume_right = 100 * (out_volume_right) / max_volume;

		// Return max of two chanel
		return std::max(out_volume_left, out_volume_right);
	}

	void set_mic(long in_volume)
	{
		snd_mixer_handle_events(mic_handle);

		long min_volume, max_volume;

		snd_mixer_selem_get_capture_volume_range(mic_element, &min_volume, &max_volume);

		// Checks for volume bounds
		if (in_volume < 0 || in_volume > 100)
			LOG_ERROR("Trying to set volume out of bounds");

		// Adjust volume to perform set operation
		in_volume = (in_volume * (max_volume - min_volume) / (99)) + min_volume;

		// Set in left and right channel
		if (snd_mixer_selem_set_capture_volume(mic_element, SND_MIXER_SCHN_FRONT_LEFT, in_volume) < 0)
		{
			snd_mixer_close(mic_handle);
			LOG_ERROR("Failed to set volume of sound element in left chanel");
		}

		if (snd_mixer_selem_set_capture_volume(mic_element, SND_MIXER_SCHN_FRONT_RIGHT, in_volume) < 0)
		{
			snd_mixer_close(mic_handle);
			LOG_ERROR("Failed to set volume of sound element in rigth chanel");
		}
	}

	long get_vol()
	{
		snd_mixer_handle_events(volume_handle);

		long min_volume, max_volume;
		long out_volume_left, out_volume_right;

		snd_mixer_selem_get_playback_volume_range(volume_element, &min_volume, &max_volume);
		
		if (snd_mixer_selem_get_playback_volume(volume_element, SND_MIXER_SCHN_FRONT_LEFT, &out_volume_left) < 0)
		{
			snd_mixer_close(volume_handle);
			LOG_ERROR("Failed to get volume of sound element in left chanel");
		}

		if (snd_mixer_selem_get_playback_volume(volume_element, SND_MIXER_SCHN_FRONT_RIGHT, &out_volume_right) < 0)
		{
			snd_mixer_close(volume_handle);
			LOG_ERROR("Failed to get volume of sound element in rigth chanel");
		}

		// Calculate real maximum volume
		max_volume -= min_volume;

		// Adjust to minimum bound
		out_volume_left -= min_volume;
		out_volume_right -= min_volume;

		// Adjust to 100% scale
		out_volume_left = 100 * (out_volume_left) / max_volume;
		out_volume_right = 100 * (out_volume_right) / max_volume;

		// Return max of two chanel
		return std::max(out_volume_left, out_volume_right);
	}

	void set_vol(long in_volume)
	{
		snd_mixer_handle_events(volume_handle);

		long min_volume, max_volume;

		snd_mixer_selem_get_playback_volume_range(volume_element, &min_volume, &max_volume);

		// Checks for volume bounds
		if (in_volume < 0 || in_volume > 100)
			LOG_ERROR("Trying to set volume out of bounds");

		// Adjust volume to perform set operation
		in_volume = (in_volume * (max_volume - min_volume) / (99)) + min_volume;

		// Set in left and right channel
		if (snd_mixer_selem_set_playback_volume(volume_element, SND_MIXER_SCHN_FRONT_LEFT, in_volume) < 0)
		{
			snd_mixer_close(volume_handle);
			LOG_ERROR("Failed to set volume of sound element in left chanel");
		}

		if (snd_mixer_selem_set_playback_volume(volume_element, SND_MIXER_SCHN_FRONT_RIGHT, in_volume) < 0)
		{
			snd_mixer_close(volume_handle);
			LOG_ERROR("Failed to set volume of sound element in rigth chanel");
		}
	}
}

//
//	DATE Metrics
//
namespace date
{
	std::string get_formated_date()
	{
		auto time_shot = time(0);

		tm* local_time_shot = localtime(&time_shot);

		std::stringstream time_ss;

		time_ss << std::put_time(local_time_shot, "%Y-%m-%d %H:%M:%S");

		return time_ss.str();
	}
}

//
//	NET Metrics
//

int main(int argc, char **argv)
{
	// const wchar_t* a = L"⡀⡄⡆⡇";

	if (battery::has_battery())
		battery::check_supplies();

	temp::init_sensors();

	audio::init_mic_connections();
	audio::init_volume_connections();

	// Main loop
	while (app_is_running)
	{
		// std::cout << "CPU: " << cpu::get_cpu_metrics() << "%" << std::endl;
		// auto [ used, total, percent ] = ram::get_ram_metrics();
		// std::cout << "RAM: " << used << " / " << total << " (" << percent << "%)" << std::endl;
		// std::cout << "TEMP: " << temp::get_cpu_temperature_metrics() << " C" << std::endl;
		// std::cout << "Date: " << date::get_formated_date() << std::endl;
		// std::cout << "Volume: " << audio::get_vol() << std::endl;
		// std::cout << "Mic Volume: " << audio::get_mic() << std::endl;

		// auto [ capacity, charging ] = battery::get_battery_metrics();
		// std::cout << "Battery:\n\t-Capacity: " << capacity << " %\n\t-Charging: " << charging << std::endl;

		std::cout << std::fixed;
		std::cout << std::setprecision(2);
		std::cout << "CPU: " << cpu::get_cpu_metrics() << "%";
		auto [ used, total, percent ] = ram::get_ram_metrics();
		std::cout << " RAM: " << used << " / " << total << " (" << percent << "%)";
		std::cout << " TEMP: " << temp::get_cpu_temperature_metrics() << " C" ;
		std::cout << " Date: " << date::get_formated_date();
		std::cout << " Volume: " << audio::get_vol();
		std::cout << " Mic Volume: " << audio::get_mic();

		auto [ capacity, charging ] = battery::get_battery_metrics();
		std::cout << " Battery:-Capacity: " << capacity << " %-Charging: " << charging << std::endl;

		// for (int i = 50; i < 100; ++i)
		// {
		// 	audio::set_vol(i);
		// 	audio::set_mic(i);
		// 	std::this_thread::sleep_for(std::chrono::milliseconds(100));
		// 	std::cout << "Volume: " << audio::get_vol() << std::endl;
		// 	std::cout << "Mic Volume: " << audio::get_mic() << std::endl;
		// }
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	return 0;
}
