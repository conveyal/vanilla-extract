CC=clang 
# CC=gcc
# add -pg for gprof, add -g for debugging symbols
CFLAGS=-Wall -std=gnu99 -O3 -g
LIBS=-lprotobuf-c -lz -lm
SOURCES=$(wildcard *.c)
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=vex

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBS) lmdb/libraries/liblmdb/liblmdb.a -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

test: $(SOURCES)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)
