CC=clang 
#CC=gcc
CFLAGS=-Wall -std=gnu99 -O3 -g # -pg for gprof
LIBS=-lprotobuf-c -lz -lrt -lm # rt is for shared memory
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
