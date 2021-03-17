# Vanilla-extract Technical Notes

For the moment this is a scratchpad, and will need to be organized.

## OSM Entity Persistence

### Original Approach
Originally we were storing in flat memory-mapped arrays on the assumption we'd load the entire planet. But we don't use most ways, and the planet has doubled in size since then.

### LMDB
Does having multiple DBIs open or doing large transactions affect the commit speed?  Can we turn off transactions entirely for this use case?

## PBF Reading

### Summary of PBF Format
PBF structure is problematic for random seeks. In practice PBF files usually contain one primitive block per file block, and each file block nodes are always dense.
Note: The term "blob" is used in the PBF spec interchangeably with "block".

### Handler / Callback Functions 
Node, way and relation callbacks were originally passed as a pointer to a struct containing all three. This is now copied to a private variable at the pbf-read module level. As the PBF reading code is refactored into simpler or shorter functions, it becomes awkward to pass this set of pointers down through layers and calls that don't even use them. Because the PBF reading is not intended to be re-entrant or called from multiple threads, it is convenient to place them in a varibale, though it is not as obvious those variables must be set when they are not a function parameter. 

There may be some performance or optimization benefit to hard-wiring these handler functions, but that would probably be premature optimization since it constrains experimentation and changes in design, and we have collected no evidence it would be faster in practice.

### Fast Forward
Standard PBF files have elements in the order nodes, then ways, then relations, with IDs in ascending order within each entity type. (Check PBF header optional and required features flags.) This order seems to be be chosen for its desirable characteristics when loading into a database. Considering the direction of references between the elements (ways reference nodes, and relations reference ways), following this standard order allows referential integrity to be checked during loading. Sorting elements by ID also enables much more efficient loading and indexing in tree data structures. However, due to the same direction of reference considerations, when filtering input we need to read the elements in the opposite order. We must first filter the ways (for example, based on their tags) in order to load only the nodes referenced by those ways.

This means seeking past the largest parts of the file (the nodes and perhaps the ways) in a file format that does not seem designed for random access. The starting positions of file blocks are not known in advance (blocks have variable size) and the contents of data blocks are opaque: they must be decompressed and decoded to discover what entity types they contain. These decompression and decoding stages are expected to be the most time consuming, as they concern the largest amount of data and must make expanded copies of that data. By skipping these steps, even if we scan over the entire file we expect to trigger reads on only the pages containing the block headers, so spend much less time waiting on IO.

Testing supports this theory. Simply decoding the block header and the block to learn its length, then moving on to the next block without decompressing the data within the block or decoding the primitive groups within the compressed data allows almost instantaneous traversal of files hundreds of megabytes in size. The speedup is so great that I would speculate that the protobuf decoder has zero-copy behavior on large binary fields like the compressed block data, and is simply returning a pointer to the original data in the disk cache pages.

However, although we're identifying the start positions of blocks, we will have no idea when we've reached the blocks containing the entity type we are interested in. It might be possible to decompress and decode only the initial section of each block to probe for its entity type. The block header and structures pointing to the OSM entities should be of fixed width. However, a variable-length string table comes before the groups of element themselves, so the number of bytes to decompress before reaching OSM entities is unknown. Probing in this way then involves decompressing an unknown amount of data before calling protobuf decoding functions that are expected to fail due to partial decoding of truncated buffers. This seems complex and error prone, and is further confounded by the fact that a block can contain more than one "primitive group" so could in theory contain a group of nodes followed by a group of ways (though in practice files usually contain one group per block), requiring decompressing and decoding a piece of much larger but still unknown size.

It seems that probing every block is not an option. As an alternative, we can decompress and decode entire blocks, but exploit the fact that blocks with the same type of entities are contiguous and use a decimation approach, 

Broadly there are two options here: pre-indexing the location of all the blocks and then performing a search that uses random access (such as a binary search) or iterating linearly over the blocks but skipping large numbers of blocks until we detect a change.

Alternatively we could perform a binary search to find the transition points between the element types. Blocks are of varying size, so the starting position of blocks is not known without iterating over them in order. To perform a binary search, we need to jump to arbitrary blocks within the file, so would first need to scan over the entire file and record the starting position of every block in the file. Note also that number of blocks is unknown before we finish the scan, it often numbers in the millions, and we will never re-use the vast majority of these addresses.

Consider though that there are only three sections to the file (nodes, ways, and relations) and the nodes are by far the most numerous (typically ten times more than ways). So the majority of the speedup comes from being able to skip over the beginning of the file.

### Slab Allocation

## OSM ID tracking

## Performance

As of 05f7a85a8de6508ce7e3c0d204cce56a54b30ee7 loading all highway and platform ways and associated nodes for all of Norway takes about 20 seconds. Process sampling with macOS activity monitor indicates a time breakdown of roughly:
- 43% handle_node, of which
    - 25% mdb_put
    - 14% roaring_bitmap_contains
- 32% protobuf_c_message_unpack
- 20% inflate

This suggests a fairly simple division of labor into two halves for multi-threading: the thread performing the decompression should also parse the messages in the decompressed data. This could actually be the main thread performing primitive block decompression/decoding, then passing the resulting PBF messages off to the secondary thread for writing to the database, locking the buffer of deflated memory (and associated allocator) until the next block is ready. This would need to be double buffered, and each decoding buffer could even include its own stack or statically allocated slab: struct {uint8_t slab[8MB]), uint8_t deflated[32MB]}). In practice about 3 MiB of the slab is observed to be used when it's reset.

## Other comments moved out of code

The problem with binary search for element type transitions is that it requires different logic for indexing and normal searching. We can instead use the standard read logic, with fast forward conditionals. Instead of indexing and then reading switch into fast forward mode. Only blocks mod N are expanded, and the first time any callback applies, we rewind to the last block where it didn't apply, switch off fast forward mode, and proceed at normal speed decompressing every block. The only problem here is that you'd need to re-fast forward if you want to hit the ways twice in a row. There's no known use case for that, and anyway the fast-forwarding is extremely fast.

Fast Forward:

1. If no callback applied and the file is more than size N, mark the current position and switch to ffwd mode.
2. If ffwd and blob number mod skip is not zero, do not decompress and just continue.
3. If a callback is applied in ffwd, switch off ffwd and rewind to marked block number and byte position.

In theory we could encounter a file that had no compressed blocks, reducing the cost of examining the blocks since they wouldn't need to be decompressed. But decoding every primitive block would still decode their string tables etc. so is expected to have a noticeable speed impact even on uncompressed data.

Maybe factor out pbf_block_iterate method with two block handlers, for block indexing and main reading operation.
Using conditional and two blocks rather than fully general purpose function pointer?
Also factor out block_unpack function for use in both normal reading and element type probing.

Check if pbf decoding code uses malloc for anything including strings or raw data chunks. Does it copy any data?
We really need to determine whether we can eliminate the calls to free_unpacked for slab-allocated PBF items.
We can also move the slab_reset calls to the top of the loop if there isn't other deallocation cleanup at the bottom.

Break out open, read, and close into separate functions.
Store PBF file offsets to each entity type when they are found, allowing repeated calls.
Convenience functions pbf_read_nodes, pbf_read_ways, pbf_read_relations 

Skipping already-decompressed node blocks without delta-decoding them anecdotally gives a 7% speedup.

