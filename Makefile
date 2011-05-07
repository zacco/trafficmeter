include Makefile.common

CFLAGS = -g -Wall
CC = gcc

all: $(PROGRAM)

trafficmeter: trafficmeter.c
	$(CC) $(CFLAGS) -o $(PROGRAM) trafficmeter.c \
		-lpcap `pkg-config --cflags gtk+-2.0` \
		`pkg-config --libs gtk+-2.0 gthread-2.0`

clean:
	rm -f trafficmeter

archive:
	tar -cvjf trafficmeter.`date +%y%m%d%H%M%S`.tar.gz trafficmeter.c Makefile*

