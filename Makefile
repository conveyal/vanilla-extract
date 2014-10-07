# CC=gcc
CC=clang
CFLAGS=-g -Wall -std=gnu99 -O3
LIBS=-lprotobuf-c -lz
SOURCES=$(wildcard *.c)
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=vex

all: $(SOURCES) $(EXECUTABLE)
	
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) $(LIBS) -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

test: $(SOURCES)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)
