vanilla-extract
===============

Clone OSM planet or large extracts, filtering during load, and perform bounding-box extracts for bulk data users. Consumes PBF format, and produces both PBF and the experimental VEX format for binary OSM data exchange.

Please note that this is an **experimental prototype mostly dating from 2014** for testing high efficiency data storage ideas tailored to very specific use cases. A more recent and production-ready project with related goals is Brandon Liu's https://github.com/protomaps/OSMExpress.

## LMDB Branch

All recent work on vanilla-extract (since 2021) is in the [lmdb branch](https://github.com/conveyal/vanilla-extract/tree/lmdb). This takes a cue from OSMExpress and uses LMDB as the underlying storage engine, while continuing to focus on filtering OSM elements during loading and extraction as an optimization for systems that only use a small fraction of OSM data, specifically routing engines. It is still primarily a testbed for ideas and performance measurement.

## This Main Branch

This main branch rarely range checks its input data or asserts invariants about its own state, and is not necessarily stable. It does some rather extreme things, such as mapping files big enough to hold every OSM node in the world directly into memory. The efficiency and feasibility of these approaches depend strongly on details of the operating system and filesystem in use.

It also contains quite a lot of hard-coded constants that must be hand-tuned to fit the data set being loaded (maximum entity IDs, common tag values etc.) This means configuration or indeed any use of this software involves editing and recompiling C source code. If this sounds crazy to you, you probably don't want to use it.

## Vex Format

In the `vexbin_*` functions in `vex.c`, this project contains the initial draft concept for a simple bulk binary OSM data exchange format referred to as "vex", described in the following article: https://blog.conveyal.com/simpler-openstreetmap-data-exchange-formats-6d43be5230e8

A much more viable second revision of that format is described at https://blog.conveyal.com/simpler-openstreetmap-data-exchange-part-ii-82afa8bdb01,
and a draft implementation of that second revision is present in [Conveyal's `osm-lib` library](https://github.com/conveyal/osm-lib/blob/master/src/main/java/com/conveyal/osmlib/VexInput.java), in `VexInput.java` and `VexOutput.java`.

## Compiling

### Precompiled dependencies

You will need zlib and protobuf-c libraries. On Ubuntu you can install them from packages:

`sudo apt-get install libprotobuf-c0-dev zlib1g-dev`

You will also need gcc, make, and the c protobuf compiler if you don't already have them:

`sudo apt-get install build-essential protobuf-c-compiler`

By default the Makefile uses Clang, a nice compiler which produces nice error messages. You can install it with:

`sudo apt-get install clang`

On MacOS using Homebrew, you can install the necessary libraries with:

`brew install protobuf-c zlib`

### Generated sources

Before compiling, you need to generate some source code from Protobuf specifications: 

```
protoc-c fileformat.proto --c_out=.
protoc-c osmformat.proto --c_out=.
```

### Submodule Dependencies

We are now using the libraries LMDB (key-value store) and CRoaring (compressed bitmaps). These are included as git submodules and statically linked into Vanilla Extract. On recent versions of git, cloning this repo should also clone the submodules. However they are not yet recursively built by our Makefile. They must be built manually before building this project. Below are some examples of how they may be built. See the individual projects for detailed build instructions.

#### CRoaring
```shell
cd CRoaring
ll
mkdir build
cd build
cmake .. -DROARING_BUILD_STATIC=ON
make && make test
```

#### LMDB
```shell
cd lmdb/libraries/liblmdb/
make
```

### Compilation
Once everything is in place, in the root of this repository you can run:

`make clean && make`


## Usage

Only store the database on a filesystem like ext3, ext4, or apfs that supports sparse files, because Vanilla Extract will create truly huge files full of zeroes. This should ideally be on a solid-state disk, as access patterns are not really optimized to be sequential or contiguous. The program itself should only need a few megabytes of memory but benefits greatly from having plenty of free memory that the OS can use as disk cache.

To load a PBF file into the database:

`./vex <database_directory> <planet.pbf>`

Loading a full planet PBF to a solid-state drive will take at least an hour, so you may want to start with a small PBF file just to experiment with making extracts. You will want to delete the contents of the database directory before you start another import. If your machine has enough memory available, you can bypass disk entirely and perform the entire operation entirely in memory by replacing specifying 'memory' (without quotes) as the database directory parameter.

Once your PBF data is loaded, to perform an extract run:

`./vex <database_directory> <min_lat> <min_lon> <max_lat> <max_lon> <output_file.pbf>`

If you specify `-` as the output file, `vex` will write to standard output.

### Usage over HTTP

`vexserver.js` provides a simple NodeJS server to run `vex` over HTTP. This is useful if you want to keep all your data
in one place, or if you only have one server with a large SSD. It requires data to already be loaded to the database,
and is used like so:

`node vexserver.js` (on ubuntu, `nodejs vexserver.js`)

Parameters are taken from the environment:
- VEX_DB: the path to the vex database to use, default '/var/osm/db/'
- VEX_CMD: the command to run vex, default 'vex'. If vex is not in your path, change to a relative or absolute path to the binary.
- VEX_HOST: the hostname to bind on, or 0.0.0.0 for all interfaces; default 0.0.0.0
- VEX_PORT: the port to server on, default 8282

## Road Ahead

* Block-oriented revision 2 of VEX format.
* Separate compression thread in PBF and VEX read/write code.
* Revise node reference storage and bitset to handle larger IDs greater than 2^32.

Remaining loose ends to provide lossless extracts:

* Retain isolated nodes that are not referenced by a way. Such nodes must be indexed alongside the ways in each grid bin.
* Dense nodes (though this is a quirk of the PBF format and may be avoided by using our native format).

Minutely synchronization:

* Apply minutely updates (HTTP fetch may be in Python, requires readers/writer locking).
* Allow testing to see if any updates have occurred within a given bounding box since it was last fetched. A last updated timestamp should be stored in each spatial index bin.
