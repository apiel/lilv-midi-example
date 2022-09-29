CC=g++ -o demo
LV2=`pkg-config --libs lilv-0` -I/usr/include/lilv-0
# SNDFILE=`pkg-config --cflags sndfile` `pkg-config --libs sndfile`
SNDFILE=`pkg-config sndfile --cflags --libs`

linux: build run

build:
	$(CC) -Wall demo.cpp $(LV2)  $(SNDFILE)

run:
	./demo
