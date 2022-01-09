LIBS = -lsensors -lasound

main: main.cpp
	g++ $< -o $@ $(LIBS)
