vanilla-extract
===============

Clone OSM planet and perform bounding-box extracts for bulk data users. Consumes and produces PBF format.

## compiling

You will need zlib and protobuf-c libraries. On ubuntu you can install them from packages:

`sudo apt-get install libprotobuf-c0-dev zlib1g-dev`

You will also need gcc and make if you don't already have them:

`sudo apt-get install build-essential`

Clang is a nice compiler which produces nice error messages. You may want to install it and modify the CC line of the makefile to use it:

`sudo apt-get install clang`

Then just make as usual:

`make clean && make`

## usage

Only store the database on a filesystem that supports sparse files (ext3 and ext4 do). Everything goes much quicker on a solid-state disk.
The program itself should only need a few megabytes of memory but benefits greatly from free memory that the OS can use as cache.

To load a PBF file into the database:

`./vex <database_directory> <planet.pbf>`

Loading a full planet PBF to a solid-state drive will take at least an hour, so you may want to start with a small PBF file just to experiment with making extracts. You will want to delete the contents of the database directory before you start another import. If your machine has enough memory available, you can bypass disk entirely and perform the entire operation entirely in memory by replacing specifying 'memory' (without quotes) as the database directory parameter.

Once your PBF data is loaded, to perform an extract run:

`./vex <database_directory> <min_lat> <min_lon> <max_lat> <max_lon> <output_file.pbf>`

If you specify `-` as the output file, `vex` will write to standard output.

### usage over http

`vexserver.js` provides a simple NodeJS server to run `vex` over HTTP. This is useful if you want to keep all your data
in one place, or if you only have one server with a large SSD. It requires data to already be loaded to the database,
and is used like so:

`node vexserver.js` (on ubuntu, `nodejs vexserver.js`)

Parameters are taken from the environment:
- VEX_DB: the path to the vex database to use, default '/var/osm/db/'
- VEX_CMD: the command to run vex, default 'vex'. If vex is not in your path, change to a relative or absolute path to the binary.
- VEX_HOST: the hostname to bind on, or 0.0.0.0 for all interfaces; default 0.0.0.0
- VEX_PORT: the port to server on, default 8282

## road ahead

Remaining loose ends to provide lossless extracts:

* Retain isolated nodes that are not referenced by a way. Such nodes must be indexed alongside the ways in each grid bin.
* Dense nodes (though this is a quirk of the PBF format and may be avoided by using our native format).

Future possibilities include:

* Keep db in sync with minutely updates
* Allow testing to see if a given bounding box has been invalidated by updates since X date
