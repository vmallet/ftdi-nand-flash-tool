CC=gcc
FTDI_INCLUDE=/usr/include/libftdi1/
CFLAGS=-Wall -g -I$(FTDI_INCLUDE)
LIBS=-L/usr/lib/ -lftdi1 -lusb-1.0

default: flash-tool
all: flash-tool

flash-tool: flash-tool.o
	gcc flash-tool.o -o flash-tool $(LIBS)

flash-tool.o: flash-tool.c
	gcc -O2 -c flash-tool.c -o flash-tool.o $(CFLAGS)

clean:
	rm -f flash-tool flash-tool.o
