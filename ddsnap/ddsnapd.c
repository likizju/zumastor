/*
 * Snapshot Metadata Server
 *
 * Daniel Phillips, Nov 2003 to May 2004
 * (c) 2003 Sistina Software Inc.
 * (c) 2004 Red Hat Software Inc.
 *
 */

#define _GNU_SOURCE /* Berserk glibc headers: O_DIRECT not defined unless _GNU_SOURCE defined */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h> // gethostbyname2_r
#include <linux/fs.h> // BLKGETSIZE
#include <popt.h>
#include "buffer.h"
#include "daemonize.h"
#include "ddsnap.h"
#include "ddsnap.common.h"
#include "ddsnapd.h"
#include "diskio.h"
#include "dm-ddsnap.h"
#include "list.h"
#include "sock.h"
#include "trace.h"

#define trace trace_off
#define jtrace trace_off
//#define BUSHY

#define DIVROUND(N, D) (((N)+(D)-1)/(D))

/*
Todo:

BTree
  * coalesce leafs/nodes for delete
  - B*Tree splitting

Allocation bitmaps
  - allocation statistics
  - Per-snapshot free space as full-tree pass
  - option to track specific snapshot(s) on the fly
  - return stats to client (on demand? always?)
  * Bitmap block radix tree - resizing
  - allocation policy

Journal
  \ allocation
  \ write commit block
  \ write target blocks
  \ recovery
  - stats, misc data in commit block?

File backing
  \ double linked list ops
  - buffer lru
  - buffer writeout policy
  - buffer eviction policy
  - verify no busy buffers between operations

Snapshot vs origin locking
  - anti-starvation measures

Message handling
  - send reply on async write completion
  - build up immediate reply in separate buffer

Snapshot handling path
  - background copyout thread
  - try AIO
  - coalesce leaves/nodes on delete
     - should wait for current queries on snap to complete
  - background deletion optimization
     - record current deletion list in superblock

Multithreading
  - separate thread for copyouts
  - separate thread for buffer flushing
  - separate thread for new connections (?)

Utilities
  - don't include anything not needed for create
  - snapshot store integrity check (snapcheck)

Failover
  \ Mark superblock active/inactive
  + upload client locks on server restart
  + release snapshot read locks for dead client
  - Examine entire tree to initialize statistics after unsaved halt

General
  \ Prevent multiple server starts on same snapshot store
  + More configurable tracing
  - Add more internal consistency checks
  - Magic number + version for superblock
  - Flesh out and audit error paths
  - Make it endian-neutral
  - Verify wordsize neutral
  - Add an on-the-fly verify path
  + strip out the unit testing gunk
  + More documentation
  - Audits and more audits
  - Memory inversion prevention
  - failed chunk alloc fails operation, no abort

Cluster integration
  + Restart/Error recovery/reporting
*/

/*
 * Miscellaneous Primitives
 */

typedef int fd_t;

/*
 * Ripped from libiddev.  It's not quite ugly enough to convince me to
 * add a new dependency on a library that nobody has yet, but it's close.
 *
 * Heavily modified so it would work with ramdisk devices.
 */
static int fd_size(fd_t fd, u64 *bytes)
{
	unsigned long sectors;

	if (ioctl(fd, BLKGETSIZE, &sectors))
		return -errno;
	*bytes = ((u64)sectors) << 9;
	return 0;
}

static void hexdump(void const *data, unsigned length)
{
	while (length) {
		int row = length < 16? length: 16;
		printf("%p: ", data);
		length -= row;
		while (row--)
			printf("%02hhx ", *(unsigned char const *)data++);
		printf("\n");
	}
}

/* BTree Operations */

/* Directory at the base of the leaf block */

struct enode
{
	u32 count;
	u32 unused;
	struct index_entry
	{
		u64 key; // note: entries[0].key never accessed
		sector_t sector; // node sector address goes here
	} entries[];
};

struct eleaf
{
	le_u16 magic;
	le_u16 version;
	le_u32 count;
	le_u64 base_chunk; // !!! FIXME the code doesn't use the base_chunk properly
	le_u64 using_mask;
	struct etree_map
	{
		le_u32 offset;
		le_u32 rchunk;
	}
	map[];
};

static inline struct enode *buffer2node(struct buffer *buffer)
{
	return (struct enode *)buffer->data;
}

static inline struct eleaf *buffer2leaf(struct buffer *buffer)
{
	return (struct eleaf *)buffer->data;
}

/* On-disk Format */

struct exception
{
	le_u64 share;
	le_u64 chunk;
};

static inline struct exception *emap(struct eleaf *leaf, unsigned i)
{
	return	(struct exception *)((char *) leaf + leaf->map[i].offset);
}

#define SB_BUSY 1

/* Journal handling */

#define JMAGIC "MAGICNUM"

struct commit_block
{
	char magic[8];
	u32 checksum;
	s32 sequence;
	u32 entries;
	u64 sector[];
} PACKED;

static sector_t journal_sector(struct superblock *sb, unsigned i)
{
	return sb->image.journal_base + (i << sb->metadata.sectors_per_alloc_bits);
}

static inline struct commit_block *buf2block(struct buffer *buf)
{
	return (void *)buf->data;
}

static unsigned next_journal_block(struct superblock *sb)
{
	unsigned next = sb->image.journal_next;

	if (++sb->image.journal_next == sb->image.journal_size)
		sb->image.journal_next = 0;

	return next;
}

static int is_commit_block(struct commit_block *block)
{
	return !memcmp(&block->magic, JMAGIC, sizeof(block->magic));
}

static u32 checksum_block(struct superblock *sb, u32 *data)
{
	int i, sum = 0;
	for (i = 0; i < sb->metadata.asi->allocsize_bits >> 2; i++)
		sum += data[i];
	return sum;
}

static struct buffer *jgetblk(struct superblock *sb, unsigned i)
{
	return getblk(sb->metadev, journal_sector(sb, i), sb->metadata.allocsize);
}

static struct buffer *jread(struct superblock *sb, unsigned i)
{
	return bread(sb->metadev, journal_sector(sb, i), sb->metadata.allocsize);
}

/*
 * For now there is only ever one open transaction in the journal, the newest
 * one, so we don't have to check for journal wrap, but just ensure that each
 * transaction stays small enough to fit in the journal.
 *
 * Since we don't have any asynchronous IO at the moment, journal commit is
 * straightforward: walk through the dirty blocks once, writing them to the
 * journal, then again, adding sector locations to the commit block.  We know
 * the dirty list didn't change between the two passes.  When ansynchronous
 * IO arrives here, this all has to be handled a lot more carefully.
 */
static void commit_transaction(struct superblock *sb)
{
// flush_buffers();
// return;
	if (list_empty(&dirty_buffers))
		return;

	struct list_head *list;

	list_for_each(list, &dirty_buffers) {
		struct buffer *buffer = list_entry(list, struct buffer, dirty_list);
		unsigned pos = next_journal_block(sb);
		jtrace(warn("journal data sector = %Lx [%u]", buffer->sector, pos););
		assert(buffer_dirty(buffer));
		write_buffer_to(buffer, journal_sector(sb, pos));
	}

	unsigned pos = next_journal_block(sb);
	struct buffer *commit_buffer = jgetblk(sb, pos);
	memset(commit_buffer->data, 0, sb->metadata.allocsize);
	struct commit_block *commit = buf2block(commit_buffer);
	*commit = (struct commit_block){ .magic = JMAGIC, .sequence = sb->image.sequence++ };

	while (!list_empty(&dirty_buffers)) {
		struct list_head *entry = dirty_buffers.next;
		struct buffer *buffer = list_entry(entry, struct buffer, dirty_list);
		jtrace(warn("write data sector = %Lx", buffer->sector););
		assert(buffer_dirty(buffer));
		assert(commit->entries < sb->max_commit_blocks);
		commit->sector[commit->entries++] = buffer->sector;
		write_buffer(buffer); // deletes it from dirty (fixme: fragile)
		// we hope the order we just listed these is the same as committed above
	}

	jtrace(warn("commit journal block [%u]", pos););
	commit->checksum = 0;
	commit->checksum = -checksum_block(sb, (void *)commit);
	write_buffer_to(commit_buffer, journal_sector(sb, pos));
	brelse(commit_buffer);
}

static int recover_journal(struct superblock *sb)
{
	struct buffer *buffer;
	typeof(((struct commit_block *)NULL)->sequence) sequence;
	int scribbled = -1, last_block = -1, newest_block = -1;
	int data_from_start = 0, data_from_last = 0;
	int size = sb->image.journal_size;
	char const *why = "";
	unsigned i;

	/* Scan full journal, find newest commit */

	for (i = 0; i < size; brelse(buffer), i++) {
		buffer = jread(sb, i);
		struct commit_block *block = buf2block(buffer);

		if (!is_commit_block(block)) {
			jtrace(warn("[%i] <data>", i););
			if (sequence == -1)
				data_from_start++;
			else
				data_from_last++;
			continue;
		}

		if (checksum_block(sb, (void *)block)) {
			warn("block %i failed checksum", i);
			hexdump(block, 40);
			if (scribbled != -1) {
				why = "Too many scribbled blocks in journal";
				goto failed;
			}

			if (newest_block != -1 && newest_block != last_block) {
				why = "Bad block not last written";
				goto failed;
			}

			scribbled = i;
			if (last_block != -1)
				newest_block = last_block;
			sequence++;
			continue;
		}

		jtrace(warn("[%i] seq=%i", i, block->sequence););

		if (last_block != -1 && block->sequence != sequence + 1) {
			int delta = sequence - block->sequence;

			if  (delta <= 0 || delta > size) {
				why = "Bad sequence";
				goto failed;
			}

			if (newest_block != -1) {
				why = "Multiple sequence wraps";
				goto failed;
			}

			if (!(scribbled == -1 || scribbled == i - 1)) {
				why = "Bad block not last written";
				goto failed;
			}
			newest_block = last_block;
		}
		data_from_last = 0;
		last_block = i;
		sequence = block->sequence;
	}

	if (last_block == -1) {
		why = "No commit blocks found";
		goto failed;
	}

	if (newest_block == -1) {
		/* test for all the legal scribble positions here */
		newest_block = last_block;
	}

	jtrace(warn("found newest commit [%u]", newest_block););
	buffer = jread(sb, newest_block);
	struct commit_block *commit = buf2block(buffer);
	unsigned entries = commit->entries;

	for (i = 0; i < entries; i++) {
		unsigned pos = (newest_block - entries + i + size) % size;
		struct buffer *databuf = jread(sb, pos);
		struct commit_block *block = buf2block(databuf);

		if (is_commit_block(block)) {
			error("data block [%u] marked as commit block", pos);
			continue;
		}

		jtrace(warn("write journal [%u] data to %Lx", pos, commit->sector[i]););
		write_buffer_to(databuf, commit->sector[i]);
		brelse(databuf);
	}
	sb->image.journal_next = (newest_block + 1 + size) % size;
	sb->image.sequence = commit->sequence + 1;
	brelse(buffer);
	return 0;

failed:
	errno = EIO; /* return a misleading error (be part of the problem) */
	error("Journal recovery failed, %s", why);
	return -1;
}

#if 0
static void _show_journal(struct superblock *sb)
{
	int i, j;
	for (i = 0; i < sb->image.journal_size; i++) {
		struct buffer *buf = jread(sb, i);
		struct commit_block *block = buf2block(buf);

		if (!is_commit_block(block)) {
			printf("[%i] <data>\n", i);
			continue;
		}

		printf("[%i] seq=%i (%i)", i, block->sequence, block->entries);
		for (j = 0; j < block->entries; j++)
			printf(" %Lx", (long long)block->sector[j]);
		printf("\n");
		brelse(buf);
	}
	printf("\n");
}

#define show_journal(sb) do { warn("Journal..."); _show_journal(sb); } while (0)
#endif

/* BTree leaf operations */

/*
 * We operate directly on the BTree leaf blocks to insert exceptions and
 * to enquire the sharing status of given chunks.  This means all the data
 * items in the block need to be properly aligned for architecture
 * independence.  To save space and to permit binary search a directory
 * map at the beginning of the block points at the exceptions stored
 * at the top of the block.  The difference between two successive directory
 * pointers gives the number of distinct exceptions for a given chunk.
 * Each exception is paired with a bitmap that specifies which snapshots
 * the exception belongs to.
 *
 * The chunk addresses in the leaf block directory are relative to a base
 * chunk to save space.  These are currently 32 bit values but may become
 * 16 bits values.  Since each is paired with a pointer into the list of
 * exceptions, 16 bit emap entries would limit the blocksize to 64K.
 *
 * A mask in the leaf block header specifies which snapshots are actually
 * encoded in the chunk.  This allows lazy deletion (almost, needs fixing)
 *
 * The leaf operations need to know the size of the block somehow.
 * Currently that is accomplished by inserting the block size as a sentinel
 * in the block directory map; this may change.
 *
 * When an exception is created by a write to the origin it is initially
 * shared by all snapshots that don't already have exceptions.  Snapshot
 * writes may later unshare some of these exceptions.
 */

/*
 * To do:
 *   - Check leaf, index structure
 *   - Mechanism for identifying which snapshots are in each leaf
 *   - binsearch for leaf, index lookup
 *   - enforce 32 bit address range within leaf
 */

static struct buffer *snapread(struct superblock const *sb, sector_t sector)
{
	return bread(sb->metadev, sector, sb->metadata.allocsize);
}

static unsigned leaf_freespace(struct eleaf *leaf);
static unsigned leaf_payload(struct eleaf *leaf);

/*
 * origin_chunk_unique: an origin logical chunk is shared unless all snapshots
 * have exceptions.
 */

static int origin_chunk_unique(struct eleaf *leaf, u64 chunk, u64 snapmask)
{
	u64 using = 0;
	u64 i, target = chunk - leaf->base_chunk;
	struct exception const *p;

	for (i = 0; i < leaf->count; i++)
		if (leaf->map[i].rchunk == target)
			goto found;
	return !snapmask;
found:
	for (p = emap(leaf, i); p < emap(leaf, i+1); p++)
		using |= p->share;

	return !(~using & snapmask);
}

/*
 * snapshot_chunk_unique: a snapshot logical chunk is shared if it has no
 * exception or has the same exception as another snapshot.  In any case
 * if the chunk has an exception we need to know the exception address.
 */

static int snapshot_chunk_unique(struct eleaf *leaf, u64 chunk, int snapshot, u64 *exception)
{
	u64 mask = 1LL << snapshot;
	unsigned i, target = chunk - leaf->base_chunk;
	struct exception const *p;

	for (i = 0; i < leaf->count; i++)
		if (leaf->map[i].rchunk == target)
			goto found;
	return 0;
found:
	for (p = emap(leaf, i); p < emap(leaf, i+1); p++)
		/* shared if more than one bit set including this one */
		if ((p->share & mask)) {
			*exception = p->chunk;
// printf("unique %Lx %Lx\n", p->share, mask);
			return !(p->share & ~mask);
		}
	return 0;
}

/*
 * add_exception_to_leaf:
 *  - cycle through map to find existing logical chunk or insertion point
 *  - if not found need to add new chunk address
 *      - move tail of map up
 *      - store new chunk address in map
 *  - otherwise
 *      - for origin:
 *          - or together all sharemaps, invert -> new map
 *      - for snapshot:
 *          - clear out bit for existing exception
 *              - if sharemap zero warn and reuse this location
 *  - insert new exception
 *      - move head of exceptions down
 *      - store new exception/sharemap
 *      - adjust map head offsets
 *
 * If the new exception won't fit in the leaf, return an error so that
 * higher level code may split the leaf and try again.  This keeps the
 * leaf-editing code complexity down to a dull roar.
 */

static unsigned leaf_freespace(struct eleaf *leaf)
{
	char *maptop = (char *)(&leaf->map[leaf->count + 1]); // include sentinel
	return (char *)emap(leaf, 0) - maptop;
}

static unsigned leaf_payload(struct eleaf *leaf)
{
	int lower = (char *)(&leaf->map[leaf->count]) - (char *)leaf->map;
	int upper = (char *)emap(leaf, leaf->count) - (char *)emap(leaf, 0);
	return lower + upper;
}

static int add_exception_to_leaf(struct eleaf *leaf, u64 chunk, u64 exception, int snapshot, u64 active)
{
	unsigned target = chunk - leaf->base_chunk;
	u64 mask = 1ULL << snapshot, sharemap;
	struct exception *ins, *exceptions = emap(leaf, 0);
	char *maptop = (char *)(&leaf->map[leaf->count + 1]); // include sentinel
	unsigned i, j, free = (char *)exceptions - maptop;

	trace(warn("chunk %Lx exception %Lx, snapshot = %i free space = %u", 
				chunk, exception, snapshot, free););

	for (i = 0; i < leaf->count; i++) // !!! binsearch goes here
		if (leaf->map[i].rchunk >= target)
			break;

	if (i == leaf->count || leaf->map[i].rchunk > target) {
		if (free < sizeof(struct exception) + sizeof(struct etree_map))
			return -EFULL;
		ins = emap(leaf, i);
		memmove(&leaf->map[i+1], &leaf->map[i], maptop - (char *)&leaf->map[i]);
		leaf->map[i].offset = (char *)ins - (char *)leaf;
		leaf->map[i].rchunk = target;
		leaf->count++;
		sharemap = snapshot == -1? active: mask;
		goto insert;
	}

	if (free < sizeof(struct exception))
		return -EFULL;

	if (snapshot == -1) {
		for (sharemap = 0, ins = emap(leaf, i); ins < emap(leaf, i+1); ins++)
			sharemap |= ins->share;
		sharemap = (~sharemap) & active;
	} else {
		for (ins = emap(leaf, i); ins < emap(leaf, i+1); ins++)
			if ((ins->share & mask)) {
				ins->share &= ~mask;
				break;
			}
		sharemap = mask;
	}
	ins = emap(leaf, i);
insert:
	memmove(exceptions - 1, exceptions, (char *)ins - (char *)exceptions);
	ins--;
	ins->share = sharemap;
	ins->chunk = exception;

	for (j = 0; j <= i; j++)
		leaf->map[j].offset -= sizeof(struct exception);

	return 0;
}

/*
 * split_leaf: Split one leaf into two approximately in the middle.  Copy
 * the upper half of entries to the new leaf and move the lower half of
 * entries to the top of the original block.
 */
static u64 split_leaf(struct eleaf *leaf, struct eleaf *leaf2)
{
	unsigned i, nhead = (leaf->count + 1) / 2, ntail = leaf->count - nhead, tailsize;
	/* Should split at middle of data instead of median exception */
	u64 splitpoint = leaf->map[nhead].rchunk + leaf->base_chunk;
	char *phead, *ptail;

	phead = (char *)emap(leaf, 0);
	ptail = (char *)emap(leaf, nhead);
	tailsize = (char *)emap(leaf, leaf->count) - ptail;

	/* Copy upper half to new leaf */
	memcpy(leaf2, leaf, offsetof(struct eleaf, map)); // header
	memcpy(&leaf2->map[0], &leaf->map[nhead], (ntail + 1) * sizeof(struct etree_map)); // map
	memcpy(ptail - (char *)leaf + (char *)leaf2, ptail, tailsize); // data
	leaf2->count = ntail;

	/* Move lower half to top of block */
	memmove(phead + tailsize, phead, ptail - phead);
	leaf->count = nhead;
	for (i = 0; i <= nhead; i++) // also adjust sentinel
		leaf->map[i].offset += tailsize;
	leaf->map[nhead].rchunk = 0; // tidy up

	return splitpoint;
}

static void merge_leaves(struct eleaf *leaf, struct eleaf *leaf2)
{
	unsigned nhead = leaf->count, ntail = leaf2->count, i;
	unsigned tailsize = (char *)emap(leaf2, ntail) - (char *)emap(leaf2, 0);
	char *phead = (char *)emap(leaf, 0), *ptail = (char *)emap(leaf, nhead);

	// move data down
	memmove(phead - tailsize, phead, ptail - phead);

	// adjust pointers
	for (i = 0; i <= nhead; i++) // also adjust sentinel
		leaf->map[i].offset -= tailsize;

	// move data from leaf2 to top
	memcpy(ptail - tailsize, (char *)emap(leaf2, 0), tailsize); // data
	memcpy(&leaf->map[nhead], &leaf2->map[0], (ntail + 1) * sizeof(struct etree_map)); // map
	leaf->count += ntail;
}

static void merge_nodes(struct enode *node, struct enode *node2)
{
	memcpy(&node->entries[node->count], &node2->entries[0], node2->count * sizeof(struct index_entry));
	node->count += node2->count;
}

static void init_leaf(struct eleaf *leaf, int block_size)
{
	leaf->magic = 0x1eaf;
	leaf->version = 0;
	leaf->base_chunk = 0;
	leaf->count = 0;
	leaf->map[0].offset = block_size;
#ifdef BUSHY
	leaf->map[0].offset = 200;
#endif
}

/*
 * Chunk allocation via bitmaps
 */
static void set_sb_dirty(struct superblock *sb)
{
	sb->flags |= SB_DIRTY;
}

static inline int get_bitmap_bit(unsigned char *bitmap, unsigned bit)
{
	return bitmap[bit >> 3] & (1 << (bit & 7));
}

static inline void set_bitmap_bit(unsigned char *bitmap, unsigned bit)
{
	bitmap[bit >> 3] |= 1 << (bit & 7);
}

static inline void clear_bitmap_bit(unsigned char *bitmap, unsigned bit)
{
	bitmap[bit >> 3] &= ~(1 << (bit & 7));
}

static u64 calc_bitmap_blocks(struct superblock *sb, u64 chunks)
{
	unsigned chunkshift = sb->metadata.asi->allocsize_bits;
	return (chunks + (1 << (chunkshift + 3)) - 1) >> (chunkshift + 3);
}

static int init_allocation(struct superblock *sb)
{
	int meta_flag = (sb->metadev != sb->snapdev);

        unsigned meta_bitmap_base_chunk = (SB_SECTOR + 2*sb->metadata.sectors_per_alloc - 1) 
		>> sb->metadata.sectors_per_alloc_bits;
	
	sb->metadata.asi->bitmap_blocks = calc_bitmap_blocks(sb, sb->metadata.asi->chunks); 
        sb->metadata.asi->bitmap_base = meta_bitmap_base_chunk << sb->metadata.sectors_per_alloc_bits;
        sb->metadata.asi->last_alloc = 0;
	
	unsigned res = meta_bitmap_base_chunk + sb->metadata.asi->bitmap_blocks + sb->image.journal_size;
        sb->metadata.asi->freechunks = sb->metadata.asi->chunks - res;
	sb->image.meta_chunks_used += res;
	
	if (meta_flag) {
		u64 snap_bitmap_base_chunk = 
			(sb->metadata.asi->bitmap_base >> sb->metadata.sectors_per_alloc_bits) + sb->metadata.asi->bitmap_blocks;
	
		sb->snapdata.asi->bitmap_blocks = calc_bitmap_blocks(sb, sb->snapdata.asi->chunks);
		sb->snapdata.asi->bitmap_base = snap_bitmap_base_chunk << sb->metadata.sectors_per_alloc_bits;
		sb->snapdata.asi->freechunks = sb->snapdata.asi->chunks;

		/* need to update freechunks in metadata */
		sb->metadata.asi->freechunks -= sb->snapdata.asi->bitmap_blocks;
		sb->image.meta_chunks_used += sb->snapdata.asi->bitmap_blocks;
	}

	sb->image.journal_base = sb->metadata.asi->bitmap_base 
		+ ((sb->metadata.asi->bitmap_blocks + 
		    (meta_flag ? sb->snapdata.asi->bitmap_blocks : 0)) 
		   << sb->metadata.sectors_per_alloc_bits);
	
	u64 chunks  = sb->metadata.asi->chunks  + (meta_flag ? sb->snapdata.asi->chunks  : 0);
	unsigned bitmaps = sb->metadata.asi->bitmap_blocks + (meta_flag ? sb->snapdata.asi->bitmap_blocks : 0);
	if (meta_flag)
		warn("metadata store size: %llu chunks (%llu sectors)", 
		     sb->metadata.asi->chunks, sb->metadata.asi->chunks << sb->metadata.sectors_per_alloc_bits);
	warn("snapshot store size: %llu chunks (%llu sectors)", 
	     chunks, chunks << sb->snapdata.sectors_per_alloc_bits);
	printf("Initializing %u bitmap blocks... ", bitmaps);
	
	unsigned i, reserved = sb->metadata.asi->chunks - sb->metadata.asi->freechunks, 
		sector = sb->metadata.asi->bitmap_base;
	for (i = 0; i < bitmaps; i++, sector += sb->metadata.sectors_per_alloc) {
		struct buffer *buffer = getblk(sb->metadev, sector, sb->metadata.allocsize);
		printf("%llx ", buffer->sector);
		memset(buffer->data, 0, sb->metadata.allocsize);
		/* Reserve bitmaps and superblock */
		if (i == 0) {
			unsigned i;
			for (i = 0; i < reserved; i++)
				set_bitmap_bit(buffer->data, i);
		}
		/* Suppress overrun allocation in partial last byte */
		if (i == bitmaps - 1 && (chunks & 7))
			buffer->data[(chunks >> 3) & (sb->metadata.allocsize - 1)] |= 0xff << (chunks & 7);
		trace_off(dump_buffer(buffer, 0, 16););
		brelse_dirty(buffer);
	}
	printf("\n");
	return 0;
}

static int free_chunk(struct superblock *sb, struct allocspace *as, chunk_t chunk)
{
	unsigned bitmap_shift = sb->metadata.asi->allocsize_bits + 3, bitmap_mask = (1 << bitmap_shift ) - 1;
	u64 bitmap_block = chunk >> bitmap_shift;

	trace(printf("free chunk %Lx\n", chunk););
	struct buffer *buffer = snapread(sb, as->asi->bitmap_base + 
			(bitmap_block << sb->metadata.sectors_per_alloc_bits));
	
	if (!buffer) {
		warn("unable to free chunk "U64FMT, chunk);
		return 0;
	}
	if (!get_bitmap_bit(buffer->data, chunk & bitmap_mask)) {
		warn("chunk %Lx already free!", (long long)chunk);
		brelse(buffer);
		return 0;
	}
	clear_bitmap_bit(buffer->data, chunk & bitmap_mask);
	brelse_dirty(buffer);
	as->asi->freechunks++;
	set_sb_dirty(sb); // !!! optimize this away
	return 1;
}

static inline void free_block(struct superblock *sb, sector_t address)
{
	if (free_chunk(sb, &sb->metadata, address >> sb->metadata.sectors_per_alloc_bits))
		sb->image.meta_chunks_used--; 
}

static inline void free_exception(struct superblock *sb, chunk_t chunk)
{
	if (free_chunk(sb, &sb->snapdata, chunk))
		sb->image.snap_chunks_used--;
}

#ifdef INITDEBUG2
static void grab_chunk(struct superblock *sb, struct allocspace *as, chunk_t chunk) // just for testing
{
	unsigned bitmap_shift = sb->metadata.asi->allocsize_bits + 3, bitmap_mask = (1 << bitmap_shift ) - 1;
	u64 bitmap_block = chunk >> bitmap_shift;

	struct buffer *buffer = snapread(sb, as->asi->bitmap_base + (bitmap_block << sb->metadata.sectors_per_alloc_bits));
	assert(!get_bitmap_bit(buffer->data, chunk & bitmap_mask));
	set_bitmap_bit(buffer->data, chunk & bitmap_mask);
	brelse_dirty(buffer);
}
#endif

static chunk_t alloc_chunk_range(struct superblock *sb, struct allocspace *as, chunk_t chunk, chunk_t range)
{
	unsigned bitmap_shift = sb->metadata.asi->allocsize_bits + 3, bitmap_mask = (1 << bitmap_shift ) - 1;
	u64 blocknum = chunk >> bitmap_shift;
	unsigned bit = chunk & 7, offset = (chunk & bitmap_mask) >> 3;
	u64 length = (range + bit + 7) >> 3;

	while (1) {
		struct buffer *buffer = 
			snapread(sb, as->asi->bitmap_base + (blocknum << sb->metadata.sectors_per_alloc_bits));
		if (!buffer)
			return -1;
		unsigned char c, *p = buffer->data + offset;
		unsigned tail = sb->metadata.allocsize - offset, n = tail > length? length: tail;

		trace_off(printf("search %u bytes of bitmap %Lx from offset %u\n", n, blocknum, offset););
		// dump_buffer(buffer, 4086, 10);

		for (length -= n; n--; p++)
			if ((c = *p) != 0xff) {
				int i, bit;
				trace_off(printf("found byte at offset %u of bitmap %Lx = %hhx\n", 
						 p - buffer->data, blocknum, c););
				for (i = 0, bit = 1;; i++, bit <<= 1)
					if (!(c & bit)) {
						chunk = i + ((p - buffer->data) << 3) + (blocknum << bitmap_shift);
						assert(!get_bitmap_bit(buffer->data, chunk & bitmap_mask));
						set_bitmap_bit(buffer->data, chunk & bitmap_mask);
						brelse_dirty(buffer);
						as->asi->freechunks--;
						set_sb_dirty(sb); // !!! optimize this away
						return chunk;
					}
			}

		brelse(buffer);
		if (!length)
			return -1;
		if (++blocknum == as->asi->bitmap_blocks)
			 blocknum = 0;
		offset = 0;
		trace_off(printf("go to bitmap %Lx\n", blocknum););
	}
}

static int delete_snapshot(struct superblock *sb, unsigned tag);
// !!! possible/practical to topsort away this forward ref?

static const struct snapshot *find_snapshot_to_delete(const struct snapshot * snaplist, u32 snapshots)
{
	const struct snapshot * snap_cand = NULL;
	int i, min_priority = 128; /* that's max prio plus one */

	for (i = snapshots - 1; i >= 0; i--)
		if (!snaplist[i].usecnt && snaplist[i].prio <= min_priority) {
			min_priority = snaplist[i].prio;
			snap_cand = &(snaplist[i]);
		}
	return snap_cand;
}

static chunk_t alloc_chunk(struct superblock *sb, struct allocspace *as)
{
	chunk_t last = as->asi->last_alloc, total = as->asi->chunks, found;
	const char *err_msg;
	do {
		if ((found = alloc_chunk_range(sb, as, last, total - last)) != -1 ||
		    (found = alloc_chunk_range(sb, as, 0, last)) != -1) {
			as->asi->last_alloc = found;
			set_sb_dirty(sb); // !!! optimize this away
			return (found);
		}
		const struct snapshot *cand_delete = find_snapshot_to_delete(sb->image.snaplist, sb->image.snapshots);
		err_msg = "unable to find a snapshot candidate to remove. Failing I/O.";
		if (!cand_delete)
			goto unable_to_delete; /* no snapshot to delete */
		warn("snapshot store full, releasing snapshot %u", cand_delete->tag);
		err_msg = "unable to release snapshot";
		if (delete_snapshot(sb, cand_delete->tag))
			goto unable_to_delete;
	} while (sb->image.snapshots);

 unable_to_delete:
	warn("%s", err_msg);
	return -1;
}

/* Snapshot Store Allocation */

static sector_t alloc_block(struct superblock *sb)
{
	chunk_t new_block;  
	if ((new_block = alloc_chunk(sb, &sb->metadata)) != -1)
		sb->image.meta_chunks_used++;	
	return  new_block == -1? new_block : new_block << sb->metadata.sectors_per_alloc_bits;
}

static u64 alloc_exception(struct superblock *sb)
{
	chunk_t new_exception;
	if ((new_exception = alloc_chunk(sb, &sb->snapdata)) != -1)
		sb->image.snap_chunks_used++;
	return new_exception;
}

static struct buffer *new_block(struct superblock *sb)
{
	// !!! handle alloc_block failure
	return getblk(sb->metadev, alloc_block(sb), sb->metadata.allocsize); // !!! possible null ptr deferenced
}

static struct buffer *new_leaf(struct superblock *sb)
{
	trace(printf("New leaf\n"););
	struct buffer *buffer = new_block(sb); 
	if (!buffer)
		return buffer;
	memset(buffer->data, 0, sb->metadata.allocsize);
	init_leaf(buffer2leaf(buffer), sb->metadata.allocsize);
	set_buffer_dirty(buffer);
	return buffer;
}

static struct buffer *new_node(struct superblock *sb)
{
	trace(printf("New node\n"););
	struct buffer *buffer = new_block(sb); 
	if (!buffer)
		return buffer;
	memset(buffer->data, 0, sb->metadata.allocsize);
	struct enode *node = buffer2node(buffer);
	node->count = 0;
	set_buffer_dirty(buffer);
	return buffer;
}

struct etree_path { struct buffer *buffer; struct index_entry *pnext; };

static struct buffer *probe(struct superblock *sb, u64 chunk, struct etree_path *path);
static void brelse_path(struct etree_path *path, unsigned levels);

static int traverse_tree_range(struct superblock *sb, chunk_t start, unsigned int leaves, void (*visit_leaf)(struct superblock *sb, struct eleaf *leaf, void *data), void (*visit_leaf_buffer)(struct superblock *sb, struct buffer *leafbuf, void *data), void *data)
{
	int levels = sb->image.etree_levels, level = -1;
	struct etree_path path[levels];
	struct buffer *nodebuf;
	struct buffer *leafbuf;
	struct enode *node;

	if (leaves != 0) {
		if (!(leafbuf = probe(sb, start, path)))
			return -ENOMEM;
		level = levels - 1;
		nodebuf = path[level].buffer;
		node = buffer2node(nodebuf);
		goto start;
	}

	while (1) {
		do {
			level++;
			nodebuf = snapread(sb, level? path[level - 1].pnext++->sector: sb->image.etree_root);
			if (!nodebuf) {
				warn("unable to read node at sector 0x%llx at level %d of tree traversal", level? path[level - 1].pnext++->sector: sb->image.etree_root, level);
				return -EIO;
			}
			node = buffer2node(nodebuf);
			path[level].buffer = nodebuf;
			path[level].pnext = node->entries;
			trace(printf("push to level %i, %i nodes\n", level, node->count););
		} while (level < levels - 1);

		trace(printf("do %i leaf nodes, level = %i\n", node->count, level););
		while (path[level].pnext < node->entries + node->count) {
			leafbuf = snapread(sb, path[level].pnext++->sector);
			if (!leafbuf) {
				warn("unable to read leaf at sector 0x%llx of tree traversal", level? path[level - 1].pnext++->sector: sb->image.etree_root);
				return -EIO;
			}

start:
			trace(printf("process leaf %Lx\n", leafbuf->sector););
			visit_leaf(sb, buffer2leaf(leafbuf), data);

			brelse(leafbuf);

			if (visit_leaf_buffer)
				visit_leaf_buffer(sb, leafbuf, data);

			if (leaves != 0 && !--leaves) {
				brelse_path(path, level + 1);
				return 0;
			}
		}

		do {
			brelse(nodebuf);
			if (!level)
				return 0;
			nodebuf = path[--level].buffer;
			node = buffer2node(nodebuf);
			trace(printf("pop to level %i, %i of %i nodes\n", level, path[level].pnext - node->entries, node->count););
		} while (path[level].pnext == node->entries + node->count);
	}
}

static int traverse_tree_chunks(struct superblock *sb, void (*visit_leaf)(struct superblock *sb, struct eleaf *leaf, void *data), void (*visit_leaf_buffer)(struct superblock *sb, struct buffer *leafbuf, void *data), void *data)
{
	return traverse_tree_range(sb, 0, 0, visit_leaf, visit_leaf_buffer, data);
}

struct gen_changelist
{
	u64 mask1;
	u64 mask2;
	struct change_list *cl;
};

static void gen_changelist_leaf(struct superblock *sb, struct eleaf *leaf, void *data)
{
	u64 mask1 = ((struct gen_changelist *)data)->mask1;
	u64 mask2 = ((struct gen_changelist *)data)->mask2;
	struct change_list *cl = ((struct gen_changelist *)data)->cl;
	struct exception const *p;
	u64 newchunk;
	int i;

	for (i = 0; i < leaf->count; i++)
		for (p = emap(leaf, i); p < emap(leaf, i+1); p++) {
			if ( ((p->share & mask2) == mask2) != ((p->share & mask1) == mask1) ) {
				newchunk = leaf->base_chunk + leaf->map[i].rchunk;
				if (append_change_list(cl, newchunk) < 0)
					warn("unable to write chunk %llu to changelist", newchunk);
				break;
			}
		}
}

static struct change_list *gen_changelist_tree(struct superblock *sb, struct snapshot const *snapshot1, struct snapshot const *snapshot2)
{
	struct gen_changelist gcl;

	gcl.mask1 = 1ULL << snapshot1->bit;
	gcl.mask2 = 1ULL << snapshot2->bit;
	if ((gcl.cl = init_change_list(sb->snapdata.asi->allocsize_bits, snapshot1->tag, snapshot2->tag)) == NULL)
		return NULL;

	traverse_tree_chunks(sb, gen_changelist_leaf, NULL, &gcl);

	return gcl.cl;
}

/* BTree debug dump */

static void show_leaf(struct eleaf *leaf)
{
	struct exception const *p;
	int i;

	printf("base chunk: %Lx and %i chunks: ", leaf->base_chunk, leaf->count);
	for (i = 0; i < leaf->count; i++) {
		printf("%x=", leaf->map[i].rchunk);
		printf("@offset:%i ", leaf->map[i].offset);
		for (p = emap(leaf, i); p < emap(leaf, i+1); p++)
			printf("%Lx/%08llx%s", p->chunk, p->share, p+1 < emap(leaf, i+1)? ",": " ");
	}
	printf("top@%i free space calc: %d payload: %d", leaf->map[i].offset, leaf_freespace(leaf), leaf_payload(leaf));
	printf("\n");
}

static void show_subtree(struct superblock *sb, struct enode *node, int levels, int indent)
{
	int i;

	printf("%*s", indent, "");
	printf("%i nodes:\n", node->count);
	for (i = 0; i < node->count; i++) {
		struct buffer *buffer = snapread(sb, node->entries[i].sector);
		if (!buffer)
			return;
		if (i)
			printf("pivot = %Lx\n", (long long)node->entries[i].key);

		if (levels)
			show_subtree(sb, buffer2node(buffer), levels - 1, indent + 3);
		else {
			printf("%*s", indent + 3, "");
			show_leaf(buffer2leaf(buffer));
		}
		brelse(buffer);
	}
}

static void show_tree(struct superblock *sb)
{
	struct buffer *buffer = snapread(sb, sb->image.etree_root);
	if (!buffer)
		return;
	show_subtree(sb, buffer2node(buffer), sb->image.etree_levels - 1, 0);
	brelse(buffer);
}

static void show_tree_leaf(struct superblock *sb, struct eleaf *leaf, void *data)
{
	show_leaf(leaf);
}

static void show_tree_range(struct superblock *sb, chunk_t start, unsigned int leaves)
{
	traverse_tree_range(sb, start, leaves, show_tree_leaf, NULL, NULL);
}

/* High Level BTree Editing */

/*
 * BTree insertion is a little hairy, as expected.  We keep track of the
 * access path in a vector of etree_path elements, each of which holds
 * a node buffer and a pointer into the buffer data giving the address at
 * which the next buffer in the path was found, which is also where a new
 * node will be inserted if necessary.  If a leaf is split we may need to
 * work all the way up from the bottom to the top of the path, splitting
 * index nodes as well.  If we split the top index node we need to add
 * a new tree level.  We have to keep track of which nodes were modified
 * and keep track of refcounts of all buffers involved, which can be quite
 * a few.
 *
 * Note that the first key of an index block is never accessed.  This is
 * because for a btree, there is always one more key than nodes in each
 * index node.  In other words, keys lie between node pointers.  We will
 * micro-optimize by placing the node count in the first key, which allows
 * a node to contain an esthetically pleasing binary number of pointers.
 * (Not done yet.)
 */

static void brelse_path(struct etree_path *path, unsigned levels)
{
	unsigned i;
	for (i = 0; i < levels; i++)
		brelse(path[i].buffer);
}

static struct buffer *probe(struct superblock *sb, u64 chunk, struct etree_path *path)
{
	unsigned i, levels = sb->image.etree_levels;
	struct buffer *nodebuf = snapread(sb, sb->image.etree_root);
	if (!nodebuf)
		return NULL;
	struct enode *node = buffer2node(nodebuf);

	for (i = 0; i < levels; i++) {
		struct index_entry *pnext = node->entries, *top = pnext + node->count;

		while (++pnext < top)
			if (pnext->key > chunk)
				break;

		path[i].buffer = nodebuf;
		path[i].pnext = pnext;
		nodebuf = snapread(sb, (pnext - 1)->sector);
		if (!nodebuf) {
			brelse_path(path, i);		
			return NULL;
		}
		node = (struct enode *)nodebuf->data;
	}
	assert(((struct eleaf *)nodebuf->data)->magic == 0x1eaf);
	return nodebuf;
}

static void insert_child(struct enode *node, struct index_entry *p, sector_t child, u64 childkey)
{
	memmove(p + 1, p, (char *)(&node->entries[0] + node->count) - (char *)p);
	p->sector = child;
	p->key = childkey;
	node->count++;
}

/* returns 0 on sucess and -errno on failure */
static int add_exception_to_tree(struct superblock *sb, struct buffer *leafbuf, u64 target, u64 exception, int snapnum, struct etree_path path[], unsigned levels)
{
	if (!add_exception_to_leaf(buffer2leaf(leafbuf), target, exception, snapnum, sb->snapmask)) {
		brelse_dirty(leafbuf);
		return 0;
	}

	trace(warn("adding a new leaf to the tree"););
	struct buffer *childbuf = new_leaf(sb);
	if (!childbuf) 
		return -ENOMEM; /* this is the right thing to do? */
	
	u64 childkey = split_leaf(buffer2leaf(leafbuf), buffer2leaf(childbuf));
	sector_t childsector = childbuf->sector;

	if (add_exception_to_leaf(target < childkey ? buffer2leaf(leafbuf): buffer2leaf(childbuf), target, exception, snapnum, sb->snapmask)) {
		warn("new leaf has no space");
		return -ENOMEM;
	}
	brelse_dirty(leafbuf);
	brelse_dirty(childbuf);

	while (levels--) {
		struct index_entry *pnext = path[levels].pnext;
		struct buffer *parentbuf = path[levels].buffer;
		struct enode *parent = buffer2node(parentbuf);

		if (parent->count < sb->metadata.alloc_per_node) {
			insert_child(parent, pnext, childsector, childkey);
			set_buffer_dirty(parentbuf);
			return 0;
		}

		unsigned half = parent->count / 2;
		u64 newkey = parent->entries[half].key;
		struct buffer *newbuf = new_node(sb); 
		if (!newbuf) 
			return -ENOMEM;
		struct enode *newnode = buffer2node(newbuf);

		newnode->count = parent->count - half;
		memcpy(&newnode->entries[0], &parent->entries[half], newnode->count * sizeof(struct index_entry));
		parent->count = half;

		if (pnext > &parent->entries[half]) {
			pnext = pnext - &parent->entries[half] + newnode->entries;
			set_buffer_dirty(parentbuf);
			parentbuf = newbuf;
			parent = newnode;
		} else set_buffer_dirty(newbuf);

		insert_child(parent, pnext, childsector, childkey);
		set_buffer_dirty(parentbuf);
		childkey = newkey;
		childsector = newbuf->sector;
		brelse(newbuf);
	}

	trace(printf("add tree level\n"););
	struct buffer *newrootbuf = new_node(sb); // !!! handle error
	if (!newrootbuf)
		return -ENOMEM;
	struct enode *newroot = buffer2node(newrootbuf);

	newroot->count = 2;
	newroot->entries[0].sector = sb->image.etree_root;
	newroot->entries[1].key = childkey;
	newroot->entries[1].sector = childsector;
	sb->image.etree_root = newrootbuf->sector;
	sb->image.etree_levels++;
	set_sb_dirty(sb);
	brelse_dirty(newrootbuf);
	return 0;
}
#define chunk_highbit ((sizeof(chunk_t) * 8) - 1)

static int finish_copyout(struct superblock *sb)
{
	if (sb->copy_chunks) {
		int is_snap = sb->source_chunk >> chunk_highbit;
		chunk_t source = sb->source_chunk & ~(1ULL << chunk_highbit);
		unsigned size = sb->copy_chunks << sb->snapdata.asi->allocsize_bits;
		trace(printf("copy %u %schunks from %Lx to %Lx\n", sb->copy_chunks, 
					is_snap? "snapshot ": "origin ", source, sb->dest_exception););
		assert(size <= sb->copybuf_size);
		if (diskread(is_snap? sb->snapdev: sb->orgdev, sb->copybuf, size, 
					source << sb->snapdata.asi->allocsize_bits) < 0)
			trace(printf("copyout death on read\n"););
		if (diskwrite(sb->snapdev, sb->copybuf, size, 
					sb->dest_exception << sb->snapdata.asi->allocsize_bits) < 0)
			trace_on(printf("copyout death on write\n"););
		sb->copy_chunks = 0;
	}
	return 0;
}

static int copyout(struct superblock *sb, chunk_t chunk, chunk_t exception)
{
#if 1
	if (sb->source_chunk + sb->copy_chunks == chunk &&
		sb->dest_exception + sb->copy_chunks == exception &&
		sb->copy_chunks < sb->copybuf_size >> sb->snapdata.asi->allocsize_bits) {
		sb->copy_chunks++;
		return 0;
	}
	finish_copyout(sb); // why do this?
	sb->copy_chunks = 1;
	sb->source_chunk = chunk;
	sb->dest_exception = exception;
	finish_copyout(sb);
#else
	int is_snap = sb->source_chunk >> chunk_highbit;
	chunk_t source = chunk & ~((1ULL << chunk_highbit) - 1);
	diskread(is_snap? sb->snapdev: sb->orgdev, sb->copybuf, sb->snapdata.allocsize, 
		 source << sb->snapdata.asi->allocsize_bits);  
	diskwrite(sb->snapdev, sb->copybuf, sb->snapdata.allocsize, exception << sb->snapdata.asi->allocsize_bits);  
#endif
	return 0;
}

/*
 * This is the bit that does all the work.  It's rather arbitrarily
 * factored into a probe and test part, then an exception add part,
 * called only if an exception for a given chunk isn't already present
 * in the Btree.  This factoring will change a few more times yet as
 * the code gets more asynchronous and multi-threaded.
 */
static chunk_t make_unique(struct superblock *sb, chunk_t chunk, int snapnum)
{
	unsigned levels = sb->image.etree_levels;
	struct etree_path path[levels + 1];
	struct buffer *leafbuf = probe(sb, chunk, path);
	chunk_t exception = 0;
	int error;
	trace(warn("chunk %Lx, snapnum %i", chunk, snapnum););

	if (!leafbuf) 
		return -1;

	if (snapnum == -1?
		origin_chunk_unique(buffer2leaf(leafbuf), chunk, sb->snapmask):
		snapshot_chunk_unique(buffer2leaf(leafbuf), chunk, snapnum, &exception))
	{
		trace_off(warn("chunk %Lx already unique in snapnum %i", chunk, snapnum););
		brelse(leafbuf);
		goto out;
	}
	u64 newex = alloc_exception(sb);
	if (newex == -1) {
		exception = newex;
		brelse(leafbuf); // !!! maybe add_exception_to_tree should not do this
		goto out;
	}

	copyout(sb, exception? (exception | (1ULL << chunk_highbit)): chunk, newex);
	if ((error = add_exception_to_tree(sb, leafbuf, chunk, newex, snapnum, path, levels)) < 0) {
		free_exception(sb, newex);
		brelse(leafbuf); /* !!! redundant? */
		warn("unable to add exception to tree: %s", strerror(-error));
		newex = -1;
	}
	exception = newex;
out:
	brelse_path(path, levels);
	return exception;
}

static int test_unique(struct superblock *sb, chunk_t chunk, int snapnum, chunk_t *exception)
{
	unsigned levels = sb->image.etree_levels;
	struct etree_path path[levels + 1];
	struct buffer *leafbuf = probe(sb, chunk, path);
	
	if (!leafbuf)
		return -1; /* not sure what to do here */
	
	trace(warn("chunk %Lx, snapnum %i", chunk, snapnum););
	int result = snapnum == -1?
		origin_chunk_unique(buffer2leaf(leafbuf), chunk, sb->snapmask):
		snapshot_chunk_unique(buffer2leaf(leafbuf), chunk, snapnum, exception);
	brelse(leafbuf);
	brelse_path(path, levels);
	return result;
}

/* Snapshot Store Superblock handling */

static u64 calc_snapmask(struct superblock *sb)
{
	u64 mask = 0;
	unsigned int i;

	for (i = 0; i < sb->image.snapshots; i++)
		mask |= 1ULL << sb->image.snaplist[i].bit;

	return mask;
}

static int tag_snapnum(struct superblock *sb, unsigned tag)
{
	unsigned int i, n = sb->image.snapshots;
	struct snapshot const *snap = sb->image.snaplist;

	for (i = 0; i < n; i++)
		if (snap[i].tag == tag)
			return snap[i].bit;

	return -1;
}

static unsigned int snapnum_tag(struct superblock *sb, unsigned bit)
{
	unsigned int i, n = sb->image.snapshots;
	struct snapshot const *snap = sb->image.snaplist;

	for (i = 0; i < n; i++)
		if (snap[i].bit == bit)
			return snap[i].tag;

	return (u32)~0UL;
}

static int create_snapshot(struct superblock *sb, unsigned snaptag)
{
	int i, snapshots = sb->image.snapshots;
	struct snapshot *snapshot;

	/* check tag not already used */
	for (i = 0; i < snapshots; i++)
		if (sb->image.snaplist[i].tag == snaptag)
			return -1;

	/* Find available snapshot bit */
	for (i = 0; i < MAX_SNAPSHOTS; i++)
		if (!(sb->snapmask & (1ULL << i)))
			goto create;
	return -EFULL;

create:
	trace_on(warn("Create snaptag %u (snapnum %i)", snaptag, i););
	snapshot = sb->image.snaplist + sb->image.snapshots++;
	*snapshot = (struct snapshot){ .tag = snaptag, .bit = i, .ctime = time(NULL) };
	sb->snapmask |= (1ULL << i);
	set_sb_dirty(sb);
	return i;
}

static void check_leaf(struct eleaf *leaf, u64 snapmask)
{
	struct exception *p;
	int i;

	for (i = 0; i < leaf->count; i++) {
		trace(printf("%x=", leaf->map[i].rchunk););
		// printf("@%i ", leaf->map[i].offset);
		for (p = emap(leaf, i); p < emap(leaf, i+1); p++) {
			trace(printf("%Lx/%08llx%s", p->chunk, p->share, p+1 < emap(leaf, i+1)? ",": " "););
			if (p->share & snapmask)
				printf("Leaf bitmap contains %016llx some snapshots in snapmask %016llx\n", p->share, snapmask);
		}
	}
	// printf("top@%i", leaf->map[i].offset);
}

struct delete_info
{
	u64 snapmask;
	int any;
};

/*
 * delete_snapshot: remove all exceptions from a given snapshot from a leaf
 * working from top to bottom of the exception list clearing snapshot bits
 * and packing the nonzero exceptions into the top of the block.  Then work
 * from bottom to top in the directory map packing nonempty entries into the
 * bottom of the map.
 */
static void _delete_snapshots_from_leaf(struct superblock *sb, struct eleaf *leaf, void *data)
{
	struct delete_info *dinfo = data;
	struct exception *p = emap(leaf, leaf->count), *dest = p;
	struct etree_map *pmap, *dmap;
	unsigned i;

	dinfo->any = 0;

	/* Scan top to bottom clearing snapshot bit and moving
	 * non-zero entries to top of block */
	for (i = leaf->count; i--;) {
		while (p != emap(leaf, i)) {
			u64 share = (--p)->share;
			dinfo->any |= share & dinfo->snapmask;
			if ((p->share &= ~dinfo->snapmask))
				*--dest = *p;
			else
				free_exception(sb, p->chunk);
		}
		leaf->map[i].offset = (char *)dest - (char *)leaf;
	}
	/* Remove empties from map */
	dmap = pmap = &leaf->map[0];
	for (i = 0; i < leaf->count; i++, pmap++)
		if (pmap->offset != (pmap + 1)->offset)
			*dmap++ = *pmap;
	dmap->offset = pmap->offset;
	dmap->rchunk = 0; // tidy up
	leaf->count = dmap - &leaf->map[0];
	check_leaf(leaf, dinfo->snapmask);
}

static void check_leaf_dirty(struct superblock *sb, struct buffer *leafbuf, void *data)
{
	struct delete_info *dinfo = data;

	if (!!dinfo->any)
		set_buffer_dirty(leafbuf);

	if (dirty_buffer_count >= (sb->image.journal_size - 1)) {
		commit_transaction(sb);
		set_sb_dirty(sb);
	}
}

static int delete_snapshots_from_leaf(struct superblock *sb, struct eleaf *leaf, u64 snapmask)
{
	struct delete_info dinfo;

	dinfo.snapmask = snapmask;

	_delete_snapshots_from_leaf(sb, leaf, &dinfo);

	return !!dinfo.any;
}

#if 1
static void delete_snapshots_from_tree(struct superblock *sb, u64 snapmask)
{
	struct delete_info dinfo;

	dinfo.snapmask = snapmask;
	dinfo.any = 0;

	trace_on(printf("delete snapshot mask %Lx\n", snapmask););

	traverse_tree_chunks(sb, _delete_snapshots_from_leaf, check_leaf_dirty, &dinfo);
}
#endif

/*
 * Delete algorithm (flesh this out)
 *
 * reached the end of an index block:
 *    try to merge with an index block in hold[]
 *    if can't merge then maybe can rebalance
 *    if can't merge then release the block in hold[] and move this block to hold[]
 *    can't merge if there's no block in hold[] or can't fit two together
 *    if can merge
 *       release and free this index block and
 *       delete from parent:
 *         if parent count zero, the grantparent key is going to be deleted, updating the pivot
 *         otherwise parent's deleted key becomes new pivot 
 */

static inline struct enode *path_node(struct etree_path path[], int level)
{
	return buffer2node(path[level].buffer);
}

static inline int finished_level(struct etree_path path[], int level)
{
	struct enode *node = path_node(path, level);
	return path[level].pnext == node->entries + node->count;
}

static void remove_index(struct etree_path path[], int level)
{
	struct enode *node = path_node(path, level);
	chunk_t pivot = (path[level].pnext)->key; // !!! out of bounds for delete of last from full index
	int count = node->count, i;

	// stomps the node count (if 0th key holds count)
	memmove(path[level].pnext - 1, path[level].pnext,
		(char *)&node->entries[count] - (char *)path[level].pnext);
	node->count = count - 1;
	--(path[level].pnext);
	set_buffer_dirty(path[level].buffer);

	// no pivot for last entry
	if (path[level].pnext == node->entries + node->count)
		return;

	// climb up to common parent and set pivot to deleted key
	// what if index is now empty? (no deleted key)
	// then some key above is going to be deleted and used to set pivot
	if (path[level].pnext == node->entries && level) {
		for (i = level - 1; path[i].pnext - 1 == path_node(path, i)->entries; i--)
			if (!i)
				return;
		(path[i].pnext - 1)->key = pivot;
		set_buffer_dirty(path[i].buffer);
	}
}


static void brelse_free(struct superblock *sb, struct buffer *buffer)
{
	brelse(buffer);
	if (buffer->count) {
		warn("free block %Lx still in use!", (long long)buffer->sector);
		return;
	}
	free_block(sb, buffer->sector);
	evict_buffer(buffer);
}

static int delete_tree_range(struct superblock *sb, u64 snapmask, chunk_t resume)
{
	int levels = sb->image.etree_levels, level = levels - 1;
	struct etree_path path[levels], hold[levels];
	struct buffer *leafbuf, *prevleaf = NULL;
	unsigned i;

	for (i = 0; i < levels; i++) // can be initializer if not dynamic array (change it?)
		hold[i] = (struct etree_path){ };

	if (!(leafbuf = probe(sb, resume, path)))
		return -ENOMEM;

	while (1) { /* in-order leaf walk */
		trace_off(show_leaf(buffer2leaf(leafbuf)););
		if (delete_snapshots_from_leaf(sb, buffer2leaf(leafbuf), snapmask))
			set_buffer_dirty(leafbuf);

		if (prevleaf) { /* try to merge this leaf with prev */
			struct eleaf *this = buffer2leaf(leafbuf);
			struct eleaf *prev = buffer2leaf(prevleaf);
			trace_off(warn("check leaf %p against %p", leafbuf, prevleaf););
			trace_off(warn("need = %i, free = %i", leaf_payload(this), leaf_freespace(prev)););
			if (leaf_payload(this) <= leaf_freespace(prev)) {
				trace_off(warn(">>> can merge leaf %p into leaf %p", leafbuf, prevleaf););
				merge_leaves(prev, this);
				remove_index(path, level);
				set_buffer_dirty(prevleaf);
				brelse_free(sb, leafbuf);
				goto keep_prev_leaf;
			}
			brelse(prevleaf);
		}
		prevleaf = leafbuf;
keep_prev_leaf:
		if (finished_level(path, level)) {
			do { /* pop and try to merge finished nodes */
				if (hold[level].buffer) {
					assert(level); /* root node can't have any prev */
					struct enode *this = path_node(path, level);
					struct enode *prev = path_node(hold, level);
					trace_off(warn("check node %p against %p", this, prev););
					trace_off(warn("this count = %i prev count = %i", this->count, prev->count););
					if (this->count <= sb->metadata.alloc_per_node - prev->count) {
						trace(warn(">>> can merge node %p into node %p", this, prev););
						merge_nodes(prev, this);
						remove_index(path, level - 1);
						set_buffer_dirty(hold[level].buffer);
						brelse_free(sb, path[level].buffer);
						goto keep_prev_node;
					}
					brelse(hold[level].buffer);
				}
				hold[level].buffer = path[level].buffer;
keep_prev_node:
				if (!level) { /* remove levels if possible */
					while (levels > 1 && path_node(hold, 0)->count == 1) {
						trace_off(warn("drop btree level"););
						sb->image.etree_root = hold[1].buffer->sector;
						brelse_free(sb, hold[0].buffer);
						levels = --sb->image.etree_levels;
						memcpy(hold, hold + 1, levels * sizeof(hold[0]));
						set_sb_dirty(sb);
					}
					brelse(prevleaf);
					brelse_path(hold, levels);
					return 0;
				}

				level--;
				trace_off(printf("pop to level %i, %i of %i nodes\n", level, path[level].pnext - path_node(path, level)->entries, path_node(path, level)->count););
			} while (finished_level(path, level));

			do { /* push back down to leaf level */
				struct buffer *nodebuf = snapread(sb, path[level++].pnext++->sector);
				if (!nodebuf) {
					brelse_path(path, level - 1); /* anything else needs to be freed? */
					return -ENOMEM;
				}
				path[level].buffer = nodebuf;
				path[level].pnext = buffer2node(nodebuf)->entries;
				trace_off(printf("push to level %i, %i nodes\n", level, path_node(path, level)->count););
			} while (level < levels - 1);
		}

		if (dirty_buffer_count >= sb->image.journal_size - 1) {
			if (dirty_buffer_count > sb->image.journal_size)
				warn("number of dirty buffers is too large for journal");
			trace_off(warn("flushing dirty buffers to disk"););
			commit_transaction(sb);
		}
		if (!(leafbuf = snapread(sb, path[level].pnext++->sector))) {
			brelse_path(path, level);
			return -ENOMEM;		
		}
	}
}

static int delete_snapshot(struct superblock *sb, unsigned tag)
{
	struct snapshot *snapshot;
	unsigned i, bit;
	int error;

	for (i = 0; i < sb->image.snapshots; i++)
		if (sb->image.snaplist[i].tag == tag)
			goto delete;
	return -1;

delete:
	snapshot = sb->image.snaplist + i;
	bit = snapshot->bit;
	trace_on(warn("Delete snaptag %u (snapnum %i)", tag, bit););
	memmove(snapshot, snapshot + 1, (char *)(sb->image.snaplist + --sb->image.snapshots) - (char *)snapshot);
	sb->snapmask &= ~(1ULL << bit);
	if ((error = delete_tree_range(sb, 1ULL << bit, 0)) < 0)
		warn("unable to delete snapshot %u: %s", tag, strerror(-error));
	set_sb_dirty(sb);
	return 0;
}

#if 0
static void show_snapshots(struct superblock *sb)
{
	unsigned int i, snapshots = sb->image.snapshots;

	printf("%u snapshots\n", snapshots);
	for (i = 0; i < snapshots; i++) {
		struct snapshot *snapshot = sb->image.snaplist + i;
		printf("snapshot %u tag %u prio %i created %x\n", 
			snapshot->bit, 
			snapshot->tag, 
			snapshot->prio, 
			snapshot->ctime);
	}
}
#endif

/* Lock snapshot reads against origin writes */

static void reply(fd_t sock, struct messagebuf *message)
{
	trace(warn("%x/%u", message->head.code, message->head.length););
	writepipe(sock, &message->head, message->head.length + sizeof(message->head));
}

#define USING 1
struct client
{
	u64 id;
	fd_t sock;
	int snap; /* snapnum/snapbit for snapshots, -1 for origin */
	u32 flags; 
};

struct pending
{
	unsigned holdcount;
	struct client *client;
	struct messagebuf message;
};

struct snaplock_wait
{
	struct pending *pending;
	struct snaplock_wait *next;
};

struct snaplock_hold
{
	struct client *client;
	struct snaplock_hold *next;
};

struct snaplock
{
	struct snaplock_wait *waitlist;
	struct snaplock_hold *holdlist;
	struct snaplock *next;
	chunk_t chunk;
};

// used to be malloc... changed to calloc
static struct snaplock *new_snaplock(struct superblock *sb)
{
	return calloc(1, sizeof(struct snaplock));
}

static struct snaplock_wait *new_snaplock_wait(struct superblock *sb)
{
	return calloc(1, sizeof(struct snaplock_wait));
}

static struct snaplock_hold *new_snaplock_hold(struct superblock *sb)
{
	return calloc(1, sizeof(struct snaplock_hold));
}

static void free_snaplock(struct superblock *sb, struct snaplock *p)
{
	free(p);
}

static void free_snaplock_hold(struct superblock *sb, struct snaplock_hold *p)
{
	free(p);
}

static void free_snaplock_wait(struct superblock *sb, struct snaplock_wait *p)
{
	free(p);
}

static unsigned snaplock_hash(struct superblock *sb, chunk_t chunk)
{
	unsigned bin = ((u32)(chunk * 3498734713U)) >> (32 - sb->snaplock_hash_bits);
	assert(bin >= 0 && bin < (1 << sb->snaplock_hash_bits));
	return bin;
}

static struct snaplock *find_snaplock(struct snaplock *list, chunk_t chunk)
{
	for (; list; list = list->next)
		if (list->chunk == chunk)
			return list;
	return NULL;
}

# ifdef DEBUG_LOCKS
static void show_locks(struct superblock *sb)
{
	unsigned n = 0, i;
	for (i = 0; i < (1 << sb->snaplock_hash_bits); i++) {
		struct snaplock *lock = sb->snaplocks[i];
		if (!lock)
			continue;
		if (!n) printf("Locks:\n");
		printf("[%03u] ", i);
		do {
			printf("chunk %Lx ", lock->chunk);
			struct snaplock_hold *hold = lock->holdlist;
			for (; hold; hold = hold->next)
				printf("held by client %Lu ", hold->client->id);
			struct snaplock_wait *wait = lock->waitlist;
			for (; wait; wait = wait->next)
				printf("wait [%02hx/%u] ", snaplock_hash(sb, (u32)wait->pending), wait->pending->holdcount);
		} while ((lock = lock->next));
		printf("\n");
		n++;
	}
	if (!n) printf("-- no locks --\n");
}
# endif

static void waitfor_chunk(struct superblock *sb, chunk_t chunk, struct pending **pending)
{
	struct snaplock *lock;

	trace(printf("enter waitfor_chunk\n"););
	if ((lock = find_snaplock(sb->snaplocks[snaplock_hash(sb, chunk)], chunk))) {
		if (!*pending) {
			// arguably we should know the client and fill it in here
			*pending = calloc(1, sizeof(struct pending));
			(*pending)->holdcount = 1;
		}
		trace(printf("new_snaplock_wait call\n"););
		struct snaplock_wait *wait = new_snaplock_wait(sb);
		wait->pending = *pending;
		wait->next = lock->waitlist;
		lock->waitlist = wait;
		(*pending)->holdcount++;
	}
	trace(printf("leaving waitfor_chunk\n"););
#ifdef DEBUG_LOCKS
	show_locks(sb);
#endif
}

static void readlock_chunk(struct superblock *sb, chunk_t chunk, struct client *client)
{
	struct snaplock **bucket = &sb->snaplocks[snaplock_hash(sb, chunk)];
	struct snaplock *lock;

	trace(printf("enter readlock_chunk\n"););
	if (!(lock = find_snaplock(*bucket, chunk))) {
		trace(printf("creating a new lock\n"););
		lock = new_snaplock(sb);
		*lock = (struct snaplock){ .chunk = chunk, .next = *bucket };
		*bucket = lock;
	}
	trace(printf("holding snaplock\n"););
	struct snaplock_hold *hold = new_snaplock_hold(sb);
	trace(printf("got the snaplock?\n"););
	hold->client = client;
	hold->next = lock->holdlist;
	lock->holdlist = hold;
	trace(printf("leaving readlock_chunk\n"););
}

static struct snaplock *release_lock(struct superblock *sb, struct snaplock *lock, struct client *client)
{
	struct snaplock *ret = lock;
	struct snaplock_hold **holdp = &lock->holdlist;

	trace(printf("entered release_lock\n"););
	while (*holdp && (*holdp)->client != client)
		holdp = &(*holdp)->next;

	if (!*holdp) {
		trace_on(printf("chunk %Lx holder %Lu not found\n", lock->chunk, client->id););
		return NULL;
	}

	/* Delete and free holder record */
	struct snaplock_hold *next = (*holdp)->next;
	free_snaplock_hold(sb, *holdp);
	*holdp = next;

	if (lock->holdlist)
		return ret;

	/* Release and delete waiters, delete lock */
	struct snaplock_wait *list = lock->waitlist;
	while (list) {
		struct snaplock_wait *next = list->next;
		assert(list->pending->holdcount);
		if (list->pending != NULL && !--(list->pending->holdcount)) {
			struct pending *pending = list->pending;
			reply(pending->client->sock, &pending->message);
			free(pending);
		}
		free_snaplock_wait(sb, list);
		list = next;
	}
	ret = lock->next;
	free_snaplock(sb, lock);

	trace(printf("leaving release_lock\n"););
	return ret;
}

static int release_chunk(struct superblock *sb, chunk_t chunk, struct client *client)
{
	trace(printf("enter release_chunk\n"););
	trace(printf("release %Lx\n", chunk););
	struct snaplock **lockp = &sb->snaplocks[snaplock_hash(sb, chunk)];

	/* Find pointer to lock record */
	while (*lockp && (*lockp)->chunk != chunk) {
		assert(lockp != &(*lockp)->next);
		lockp = &(*lockp)->next;
	}
	struct snaplock *next, *lock = *lockp;

	if (!lock) {
		trace_on(printf("chunk %Lx not locked\n", chunk););
		return -1;
	}

	next = release_lock(sb, lock, client);
	*lockp = next;
	if (!next)
		return -2;

	trace(printf("release_chunk returning 0\n next lock %p\n",next););
	return 0;
}

/* Build up a response as a list of chunk ranges */

struct addto
{
	unsigned count;
	chunk_t firstchunk;
	chunk_t nextchunk;
	struct rwmessage *reply;
	shortcount *countp;
	chunk_t *top;
	char *lim;
};

static void check_response_full(struct addto *r, unsigned bytes)
{
	if ((char *)r->top < r->lim - bytes)
		return;
	error("Need realloc");
}

static void addto_response(struct addto *r, chunk_t chunk)
{
	trace(printf("inside addto_response\n"););
	if (chunk != r->nextchunk) {
		if (r->top) {
			trace(warn("finish old range"););
			*(r->countp) = (r->nextchunk -  r->firstchunk);
		} else {
			trace(warn("alloc new reply"););
			r->reply = (void *) malloc(sizeof(struct messagebuf));
			r->top = (chunk_t *)(((char *)r->reply) + sizeof(struct head) + offsetof(struct rw_request, ranges));
			r->lim = ((char *)r->reply) + maxbody;
			r->count++;
		}
		trace(warn("start new range"););
		check_response_full(r, 2*sizeof(chunk_t));
		r->firstchunk = *(r->top)++ = chunk;
		r->countp = (shortcount *)r->top;
		r->top = (chunk_t *)(((shortcount *)r->top) + 1);
	}
	r->nextchunk = chunk + 1;
	trace(printf("leaving addto_response\n"););
}

static int finish_reply_(struct addto *r, unsigned code, unsigned id)
{
	if (!r->countp)
		return 0;

	*(r->countp) = (r->nextchunk -  r->firstchunk);
	r->reply->head.code = code;
	r->reply->head.length = (char *)r->top - (char *)r->reply - sizeof(struct head);
	r->reply->body.id = id;
	r->reply->body.count = r->count;
	return 1;
}

static void finish_reply(int sock, struct addto *r, unsigned code, unsigned id)
{
	if (finish_reply_(r, code, id)) {
		trace(printf("sending reply... "););
		reply(sock, (struct messagebuf *)r->reply);
		trace(printf("done sending reply\n"););
	}
	free(r->reply);
}

/* Initialization, State load/save */

static void setup_alloc_sb(struct superblock *sb, u32 bs_bits, u32 cs_bits)
{
	sb->metadata.asi = &(METADATA_ALLOC(sb));
	sb->snapdata.asi = &(SNAPDATA_ALLOC(sb));

	if (sb->metadev == sb->snapdev)
		assert(bs_bits == cs_bits);

	sb->metadata.asi->allocsize_bits = bs_bits;
	sb->snapdata.asi->allocsize_bits = cs_bits;
	
	sb->metadata.allocsize = 1 << bs_bits;
	sb->snapdata.allocsize = 1 << cs_bits;
	sb->metadata.sectors_per_alloc_bits = bs_bits - SECTOR_BITS;
	sb->snapdata.sectors_per_alloc_bits = cs_bits - SECTOR_BITS;
	sb->metadata.sectors_per_alloc = 1 << (bs_bits - SECTOR_BITS);
	sb->snapdata.sectors_per_alloc = 1 << (cs_bits - SECTOR_BITS);

	sb->metadata.alloc_per_node = 
		(sb->metadata.allocsize - offsetof(struct enode, entries)) / sizeof(struct index_entry);
#ifdef BUSY
	sb->metadata.alloc_per_node = 10;
#endif

}

static void setup_sb(struct superblock *sb, u32 bs_bits, u32 cs_bits)
{
	int err;

	setup_alloc_sb(sb, bs_bits, cs_bits);
	sb->copybuf_size = 32 * sb->snapdata.allocsize;
	if ((err = posix_memalign((void **)&(sb->copybuf), 4096, sb->copybuf_size)))
	    error("unable to allocate buffer for copyout data: %s", strerror(err));
	sb->snapmask = 0;
	sb->flags = 0;

	sb->max_commit_blocks = (sb->metadata.allocsize - sizeof(struct commit_block)) / sizeof(sector_t);

	unsigned snaplock_hash_bits = 8;
	sb->snaplock_hash_bits = snaplock_hash_bits;
	sb->snaplocks = (struct snaplock **)calloc(1 << snaplock_hash_bits, sizeof(struct snaplock *));

}

static void load_sb(struct superblock *sb)
{
	if (diskread(sb->metadev, &sb->image, SB_SIZE, SB_SECTOR << SECTOR_BITS) < 0)
		error("Unable to read superblock: %s", strerror(errno));
	assert(!memcmp(sb->image.magic, SB_MAGIC, sizeof(sb->image.magic)));
	setup_sb(sb, METADATA_ALLOC(sb).allocsize_bits, SNAPDATA_ALLOC(sb).allocsize_bits);
	sb->snapmask = calc_snapmask(sb);
	trace(printf("Active snapshot mask: %016llx\n", sb->snapmask););
}

static void save_sb(struct superblock *sb)
{
	if (sb->flags & SB_DIRTY) {
		if (diskwrite(sb->metadev, &sb->image, SB_SIZE , SB_SECTOR << SECTOR_BITS) < 0)
			warn("Unable to write superblock to disk: %s", strerror(errno));
		sb->flags &= ~SB_DIRTY;
	}
}

static void save_state(struct superblock *sb)
{
	flush_buffers();
	save_sb(sb);
}

/*
 * This source compiles either the snapshot server or the snapshot store setup
 * utility, depending on whether the macro variable CREATE is defined.
 *
 * I'll leave all the testing hooks lying around in the main routine for now,
 * since the low level components still tend to break every now and then and
 * require further unit testing.
 */

int init_snapstore(struct superblock *sb, u32 js_bytes, u32 bs_bits, u32 cs_bits)
{
	int i, error;
	
	sb->image = (struct disksuper){ .magic = SB_MAGIC };
	setup_sb(sb, bs_bits, cs_bits);
	sb->image.etree_levels = 1;
	sb->image.create_time = time(NULL);

	u64 size;
	if ((error = fd_size(sb->snapdev, &size)) < 0) {
		warn("Error %i: %s determining snapshot store size", error, strerror(-error));
		return error;
	}
	sb->snapdata.asi->chunks = size >> sb->snapdata.asi->allocsize_bits;
	if ((error = fd_size(sb->metadev, &size)) < 0) {
		warn("Error %i: %s determining metadata store size", error, strerror(-error));
		return error;
	}
	sb->metadata.asi->chunks = size >> sb->metadata.asi->allocsize_bits;
	if ((error = fd_size(sb->orgdev, &size)) < 0) {
		warn("Error %i: %s determining origin volume size", errno, strerror(-errno));
		return error;
	}
	sb->image.orgsectors = size >> sb->snapdata.asi->allocsize_bits;
	sb->image.orgsectors <<= sb->snapdata.asi->allocsize_bits;
	sb->image.orgsectors >>= SECTOR_BITS;
	sb->image.orgoffset  = 0; //!!! FIXME: shouldn't always assume offset starts at 0

	trace_on(printf("cs_bits %u\n", sb->snapdata.asi->allocsize_bits););
	u32 chunk_size = 1 << sb->snapdata.asi->allocsize_bits, js_chunks = DIVROUND(js_bytes, chunk_size);
	trace_on(printf("chunk_size is %u & js_chunks is %u\n", chunk_size, js_chunks););

	sb->image.journal_size = js_chunks;
	sb->image.journal_next = 0;
	sb->image.sequence = sb->image.journal_size;
	if ((error = init_allocation(sb)) < 0) {
		warn("Error: Unable to initialize allocation information");
		return error;
	}
	set_sb_dirty(sb);

	for (i = 0; i < sb->image.journal_size; i++) {
		struct buffer *buffer = jgetblk(sb, i);
		memset(buffer->data, 0, sb->metadata.allocsize);
		struct commit_block *commit = (struct commit_block *)buffer->data;
		*commit = (struct commit_block){ .magic = JMAGIC, .sequence = i };
#ifdef TEST_JOURNAL
		commit->sequence = (i + 3) % (sb->image.journal_size);
#endif
		commit->checksum = -checksum_block(sb, (void *)commit);
		brelse_dirty(buffer);
	}
#ifdef TEST_JOURNAL
	show_journal(sb);
	show_tree(sb);
	flush_buffers();
	recover_journal(sb);
	show_buffers();
#endif

#ifdef INITDEBUG1
	printf("chunk = %Lx\n", alloc_chunk_range(sb, sb->image.chunks - 1, 1));
//	struct buffer *buffer = snapread(sb, sb->image.bitmap_base + 3 * 8);
//	dump_buffer(buffer, 4090, 6);
	return 0;
#endif

#ifdef INITDEBUG2
	grab_chunk(sb, 32769);
	struct buffer *buffer = snapread(sb, sb->image.bitmap_base + 8);
	printf("sector %Lx\n", buffer->sector);
	free_chunk(sb, 32769);
	return 0;
#endif

	struct buffer *leafbuf = new_leaf(sb);
	struct buffer *rootbuf = new_node(sb);
	buffer2node(rootbuf)->count = 1;
	buffer2node(rootbuf)->entries[0].sector = leafbuf->sector;
	sb->image.etree_root = rootbuf->sector;

#ifdef INITDEBUG3
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
//	free_chunk(sb, 23);
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	printf("chunk = %Lx\n", alloc_chunk(sb));
	return 0;
#endif

	brelse_dirty(rootbuf);
	brelse_dirty(leafbuf);
#ifdef INITDEBUG4
	struct buffer *leafbuf1 = new_leaf(sb);
	struct buffer *leafbuf2 = new_leaf(sb);
	struct eleaf *leaf1 = buffer2leaf(leafbuf1);
	struct eleaf *leaf2 = buffer2leaf(leafbuf2);
	init_leaf(leaf1, 256);
	init_leaf(leaf2, 256);
	add_exception_to_leaf(leaf1, 0x111, 0x11, 0, 3);
	add_exception_to_leaf(leaf2, 0x222, 0x11, 1, 3);
	add_exception_to_leaf(leaf2, 0x333, 0x33, 1, 3);
	show_leaf(leaf1);
	show_leaf(leaf2);
	merge_leaves(leaf1, leaf2);
	show_leaf(leaf1);
	return 0;
#endif
#ifdef TEST_JOURNAL
	show_buffers();
	show_dirty_buffers();
	commit_transaction(sb);
	flush_buffers();
	evict_buffers();

	show_journal(sb);
	show_tree(sb);
	recover_journal(sb);
	flush_buffers();
	evict_buffers();
	show_tree(sb);
#endif
	save_state(sb);
	free(sb->copybuf);
	free(sb->snaplocks);
	
#ifdef INITDEBUG5
	printf("Let's try to load the superblock again\n");
	load_sb(sb);
	printf("snapshot dev %u\n", sb->snapdev);
#endif
	return 0;
}

// Expand snapshot store:
//   Calculate num bitmap blocks for new size
//   Copy bitmap blocks to current top
//   Clear new bitmap blocks
//   Reserve new bitmap blocks
//   Clear remainder bits in old last bitmap byte
//   Set remainder bits in new last bitmap byte
//   Set new bitmap base and chunks count


/* need to fix and test expand_snapstore */

#if 0
static void expand_snapstore(struct superblock *sb, u64 newchunks)
{
	u64 oldchunks = sb->image.chunks;
	unsigned oldbitmaps = sb->image.bitmap_blocks;
	unsigned newbitmaps = calc_bitmap_blocks(sb, newchunks);
	unsigned blocksize = sb->blocksize;
	unsigned blockshift = sb->image.blocksize_bits;
	u64 oldbase = sb->image.bitmap_base << SECTOR_BITS;
	u64 newbase = sb->image.bitmap_base << SECTOR_BITS;

	int i;
	for (i = 0; i < oldbitmaps; i++) {
		// do it one block at a time for now !!! sucks
		// maybe should do copy with bread/write?
		diskread(sb->snapdev, sb->copybuf, blocksize, oldbase + (i << blockshift));  // 64 bit!!!
		diskwrite(sb->snapdev, sb->copybuf, blocksize, newbase + (i << blockshift));  // 64 bit!!!
	}

	if ((oldchunks & 7)) {
		sector_t sector = (oldbase >> SECTOR_BITS) + ((oldbitmaps - 1) << sb->sectors_per_block_bits);
		struct buffer *buffer = getblk(sb->snapdev, sector, blocksize);
		buffer->data[(oldchunks >> 3) & (blocksize - 1)] &= ~(0xff << (oldchunks & 7));
		brelse_dirty(buffer);
	}

	for (i = oldbitmaps; i < newbitmaps; i++) {
		struct buffer *buffer = getblk(sb->snapdev, newbase >> SECTOR_BITS, blocksize);
		memset(buffer->data, 0, sb->blocksize);
		/* Suppress overrun allocation in partial last byte */
		if (i == newbitmaps - 1 && (newchunks & 7))
			buffer->data[(newchunks >> 3) & (blocksize - 1)] |= 0xff << (newchunks & 7);
		brelse_dirty(buffer);
	}

	for (i = 0; i < newbitmaps; i++) {
		grab_chunk(sb, (newbase >> blockshift) + i); // !!! assume blocksize = chunksize
	}

	sb->image.bitmap_base = newbase >> SECTOR_BITS;
	sb->image.chunks = newchunks;
	save_state(sb);
}
#endif

#if 0
static int client_locks(struct superblock *sb, struct client *client, int check)
{
	int i;

	for (i = 0; i < (1 << sb->snaplock_hash_bits); i++) {
		struct snaplock **lockp = &sb->snaplocks[i];

		while (*lockp) {
			struct snaplock_hold *hold;

			for (hold = (*lockp)->holdlist; hold; hold = hold->next)
				if (hold->client == client) {
					if (check)
						return 1;
					*lockp = release_lock(sb, *lockp, client);
					goto next;
				}
			lockp = &(*lockp)->next;
next:
			continue;
		}
	}
	return 0;
}

#define check_client_locks(x, y) client_locks(x, y, 1)
#define free_client_locks(x, y) client_locks(x, y, 0)

#endif

static struct snapshot *valid_snaptag(struct superblock *sb, u32 tag)
{
	int i, n = sb->image.snapshots;
	struct snapshot *snap = sb->image.snaplist;

	for (i = 0; i < n; i++)
		if (snap[i].tag == tag)
			return &snap[i];

	return NULL;
}

static struct snapshot *valid_snapnum(struct superblock *sb, int snapnum)
{
	unsigned int i, n = sb->image.snapshots;
	struct snapshot *snap = sb->image.snaplist;

	for (i = 0; i < n; i++)
		if (snap[i].bit == snapnum)
			return &snap[i];

	return NULL;
}

static unsigned int max_snapbit(struct snapshot const *snaplist, unsigned int snapshots)
{
	unsigned int i;
	unsigned int max = 0;

	for (i = 0; i < snapshots; i++)
		if (snaplist[i].bit > max)
			max = snaplist[i].bit;

	return max;
}

/* A very simple-minded implementation.  You can do it in very
 * few operations with whole-register bit twiddling but I assume
 * that we can juse find a macro somewhere which works.
 *  AKA hamming weight, sideways add
 */
static unsigned int popcount(u64 num)
{
	unsigned count = 0;

	for (; num; num >>= 1)
		if (num & 1)
			count++;

	return count;
}

static void calc_sharing(struct superblock *sb, struct eleaf *leaf, void *data)
{
	uint64_t **share_table = data;
	struct exception const *p;
	unsigned bit;
	unsigned int share_count;
	int i;

	for (i = 0; i < leaf->count; i++)
		for (p = emap(leaf, i); p < emap(leaf, i+1); p++) {
			share_count = popcount(p->share) - 1;

			for (bit = 0; bit < MAX_SNAPSHOTS; bit++)
				if (p->share & (1ULL << (u64)bit))
					share_table[bit][share_count]++;
		}
}

static struct status *get_snap_status(struct status_message *message, int snap)
{
	struct status *status;

	status = (struct status *)(message->status_data +
				snap * (sizeof(struct status) +
				message->num_columns * sizeof(status->chunk_count[0])));

	return status;
}


/*
 * Responses to IO requests take two quite different paths through the
 * machinery:
 *
 *   - Origin write requests are just sent back with their message
 *     code changed, unless they have to wait for a snapshot read
 *     lock in which case the incoming buffer is copied and the
 *     response takes a kafkaesque journey through the read locking
 *     beaurocracy.
 *
 *   - Responses to snapshot read or write requests have to be built
 *     up painstakingly in allocated buffers, keeping a lot of state
 *     around so that they end up with a minimum number of contiguous
 *     chunk ranges.  Once complete they can always be sent
 *     immediately.
 *
 * To mess things up further, snapshot read requests can return both
 * a list of origin ranges and a list of snapshot store ranges.  In
 * the latter case the specific snapshot store chunks in each logical
 * range are also returned, because they can (normally will) be
 * discontiguous.  This goes back to the client in two separate
 * messages, on the theory that the client will find it nice to be
 * able to process the origin read ranges and snapshot read chunks
 * separately.  We'll see how good an idea that is.
 *
 * The implementation ends up looking nice and tidy, but appearances
 * can be deceiving.
 */
static int incoming(struct superblock *sb, struct client *client)
{
	struct messagebuf message;
	unsigned sock = client->sock;
	int i, j, err;

	if ((err = readpipe(sock, &message.head, sizeof(message.head))))
		goto pipe_error;
	trace(warn("%x/%u", message.head.code, message.head.length););
	if (message.head.length > maxbody)
		goto message_too_long;
	if ((err = readpipe(sock, &message.body, message.head.length)))
		goto pipe_error;

	switch (message.head.code) {
	case QUERY_WRITE:
		if (client->snap == -1) {
			struct pending *pending = NULL;
			struct rw_request *body = (struct rw_request *)message.body;
			struct chunk_range *p = body->ranges;
			chunk_t chunk;
			if (message.head.length < sizeof(*body))
				goto message_too_short;

			trace(printf("origin write query, %u ranges\n", body->count););
			message.head.code = ORIGIN_WRITE_OK;
			for (i = 0; i < body->count; i++, p++)
				for (j = 0, chunk = p->chunk; j < p->chunks; j++, chunk++) {
					chunk_t exception = make_unique(sb, chunk, -1);
					if (exception == -1) {
						warn("ERROR: unable to perform copyout during origin write.");
						message.head.code = ORIGIN_WRITE_ERROR;
					}
					if (exception)
						waitfor_chunk(sb, chunk, &pending);
				}
			finish_copyout(sb);
			commit_transaction(sb);

			if (pending) {
				pending->client = client;
				memcpy(&pending->message, &message, message.head.length + sizeof(struct head));
				pending->holdcount--;
				break;
			}
			reply(sock, &message);
			break;
		}

		struct rw_request *body = (struct rw_request *)message.body;
		if (message.head.length < sizeof(*body))
			goto message_too_short;
		trace(printf("snapshot write request, %u ranges\n", body->count););
		struct addto snap = { .nextchunk = -1 };
		u32 ret_msgcode = SNAPSHOT_WRITE_OK;
		for (i = 0; i < body->count; i++)
			for (j = 0; j < body->ranges[i].chunks; j++) {
				chunk_t chunk = body->ranges[i].chunk + j;
				chunk_t exception = make_unique(sb, chunk, client->snap);
				if (exception == -1) {
					warn("ERROR: unable to perform copyout during snapshot write.");
					ret_msgcode = SNAPSHOT_WRITE_ERROR;
				}
				trace(printf("exception = %Lx\n", exception););
					addto_response(&snap, chunk);
				check_response_full(&snap, sizeof(chunk_t));
				*(snap.top)++ = exception;
			}
		finish_copyout(sb);
		commit_transaction(sb);
		finish_reply(client->sock, &snap, ret_msgcode, body->id);
		break;
	case QUERY_SNAPSHOT_READ:
	{
		struct rw_request *body = (struct rw_request *)message. body;
		if (message.head.length < sizeof(*body))
			goto message_too_short;
		trace(printf("snapshot read request, %u ranges\n", body->count););
		struct addto snap = { .nextchunk = -1 }, org = { .nextchunk = -1 };

		for (i = 0; i < body->count; i++)
			for (j = 0; j < body->ranges[i].chunks; j++) {
				chunk_t chunk = body->ranges[i].chunk + j, exception = 0;
				trace(warn("read %Lx", chunk););
				test_unique(sb, chunk, client->snap, &exception);
				if (exception) {
					trace(warn("read exception %Lx", exception););
					addto_response(&snap, chunk);
					check_response_full(&snap, sizeof(chunk_t));
					*(snap.top)++ = exception;
				} else {
					trace(warn("read origin %Lx", chunk););
					addto_response(&org, chunk);
					trace(printf("locking chunk %Lx\n", chunk););
					readlock_chunk(sb, chunk, client);
				}
			}
		//!!! when can these reads fail?
		finish_reply(client->sock, &org, SNAPSHOT_READ_ORIGIN_OK, body->id);
		finish_reply(client->sock, &snap, SNAPSHOT_READ_OK, body->id);
		break;
	}
	case FINISH_SNAPSHOT_READ:
	{
		struct rw_request *body = (struct rw_request *)message.body;
		if (message.head.length < sizeof(*body))
			goto message_too_short;
		trace(printf("finish snapshot read, %u ranges\n", body->count););

		for (i = 0; i < body->count; i++)
			for (j = 0; j < body->ranges[i].chunks; j++)
				release_chunk(sb, body->ranges[i].chunk + j, client);

		break;
	}
	case IDENTIFY:
	{
		u32 tag      = ((struct identify *)message.body)->snap;
		sector_t off = ((struct identify *)message.body)->off;
		sector_t len = ((struct identify *)message.body)->len;
		u32 err = 0; /* success */
		unsigned int error_len;
		char err_msg[MAX_ERRMSG_SIZE];

#ifdef DEBUG_ERROR
		snprintf(err_msg, MAX_ERRMSG_SIZE, "Debug mode to test error messages");
		err_msg[MAX_ERRMSG_SIZE-1] = '\0';
		goto identify_error;
#endif
		
		client->id = ((struct identify *)message.body)->id;
		client->snap = (tag == (u32)~0UL) ? -1 : tag_snapnum(sb, tag);
		client->flags = USING;
		
		trace(fprintf(stderr, "got identify request, setting id="U64FMT" snap=%i (tag=%u), sending chunksize_bits=%u\n", client->id, client->snap, tag, sb->image.chunksize_bits););
		warn("client id %llu, snaptag %u (snapnum %i)", client->id, tag, client->snap);

		if (client->snap != -1) {
			struct snapshot *snap_info;

			if (!(snap_info = valid_snaptag(sb, tag))) {
				warn("Snapshot tag %u is not valid", tag);
				snprintf(err_msg, MAX_ERRMSG_SIZE, "Snapshot tag %u is not valid", tag);
				err_msg[MAX_ERRMSG_SIZE-1] = '\0'; // make sure it's null terminated
				err = ERROR_INVALID_SNAPSHOT;
				goto identify_error;
			}
			u32 new_usecnt = snap_info->usecnt + 1;
			if (new_usecnt < snap_info->usecnt) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "Usecount overflow.");
				err_msg[MAX_ERRMSG_SIZE-1] = '\0'; // make sure it's null terminated
				err = ERROR_USECOUNT;
				goto identify_error;
			}
			
			snap_info->usecnt = new_usecnt;
		}

		if (len != sb->image.orgsectors) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "volume size mismatch for snapshot %u", tag);
			err_msg[MAX_ERRMSG_SIZE-1] = '\0'; // make sure it's null terminated
			err = ERROR_SIZE_MISMATCH;
			goto identify_error;
		}
		if (off != sb->image.orgoffset) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "volume offset mismatch for snapshot %u", tag);
			err_msg[MAX_ERRMSG_SIZE-1] = '\0'; // make sure it's null terminated
			err = ERROR_OFFSET_MISMATCH;
			goto identify_error;
		}
		
		if (outbead(sock, IDENTIFY_OK, struct identify_ok, 
				 .chunksize_bits = sb->snapdata.asi->allocsize_bits) < 0)
			warn("unable to reply to IDENTIFY message");
		break;

	identify_error:
		error_len = sizeof(struct identify_error) + strlen(err_msg) + 1;	  
		if (outhead(sock, IDENTIFY_ERROR, error_len) < 0 ||
			writepipe(sock, &err, sizeof(struct identify_error)) < 0 ||
			writepipe(sock, err_msg, error_len - sizeof(struct identify_error)) < 0)
			warn("unable to reply to IDENTIFY message with error");
		break;
		
	}
	case UPLOAD_LOCK:
		break;

	case FINISH_UPLOAD_LOCK:
		break;

	case CREATE_SNAPSHOT:
	{
#ifdef DEBUG_ERROR
		goto create_error;
#endif
		if (create_snapshot(sb, ((struct create_snapshot *)message.body)->snap) < 0)
			goto create_error;
		save_state(sb);
		if (outbead(sock, CREATE_SNAPSHOT_OK, struct { }) < 0)
			warn("unable to reply to create snapshot message");
		break;
	create_error:
		if (outbead(sock, CREATE_SNAPSHOT_ERROR, struct { }) < 0) //!!! return message
			warn("unable to send error for create snapshot message");
		break;
	}
	case DELETE_SNAPSHOT:
	{
#ifdef DEBUG_ERROR
		goto delete_error;
#endif
		if (delete_snapshot(sb, ((struct create_snapshot *)message.body)->snap) < 0)
			goto delete_error;
		save_state(sb);
		if (outbead(sock, DELETE_SNAPSHOT_OK, struct { }) < 0)
			warn("unable to reply to delete snapshot message");
		break;
	delete_error:
		if (outbead(sock, DELETE_SNAPSHOT_ERROR, struct {})  < 0) //!!! return message
			warn("unable to send error for delete snapshot message");
		break;
	}
	case INITIALIZE_SNAPSTORE:
	{
		/* FIXME: init_snapstore takes more arguments now */
		warn("Improper initialization.");
		init_snapstore(sb, DEFAULT_JOURNAL_SIZE, 
			       SECTOR_BITS + SECTORS_PER_BLOCK, SECTOR_BITS + SECTORS_PER_BLOCK);
		break;
	}
	case DUMP_TREE:
	{
		show_tree(sb);
		break;
	}
	case START_SERVER:
	{
		warn("Activating server");
		load_sb(sb);
		if (sb->image.flags & SB_BUSY) {
			warn("Server was not shut down properly");
			jtrace(show_journal(sb););
			recover_journal(sb);
		} else {
			sb->image.flags |= SB_BUSY;
			set_sb_dirty(sb);
			save_sb(sb);
		}
		break;
	}
	case LIST_SNAPSHOTS:
	{
		struct snapshot *snap = sb->image.snaplist;
		unsigned int ns = sb->image.snapshots;

		outhead(sock, SNAPSHOT_LIST, sizeof(int) + ns * sizeof(struct snapinfo));
		fdwrite(sock, &ns, sizeof(int));

		unsigned int i;

		for (i = 0; i < ns; i++) {
			fdwrite(sock, &(struct snapinfo){ 
					.snap   = snap[i].tag,
					.prio   = snap[i].prio,
					.ctime  = snap[i].ctime,
					.usecnt = snap[i].usecnt},
				sizeof(struct snapinfo));
		}
		break;
	}
	case PRIORITY:
	{
		u32 tag = ((struct priority_info *)message.body)->snap;
		s8 prio  = ((struct priority_info *)message.body)->prio;
		struct snapshot *snap_info;
		uint32_t err = 0;
		char err_msg[MAX_ERRMSG_SIZE];
		unsigned int err_len;

#ifdef DEBUG_ERROR
		snprintf(err_msg, MAX_ERRMSG_SIZE, "Debug mode to test error messages");
		err_msg[MAX_ERRMSG_SIZE-1] = '\0';
		goto prio_error;
#endif
		if (tag == (u32)~0UL) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Can not set priority for origin");
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_INVALID_SNAPSHOT;
			goto prio_error;
		}
		if (!(snap_info = valid_snaptag(sb, tag))) {
			warn("Snapshot tag %u is not valid", tag);
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Snapshot tag %u is not valid", tag);
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_INVALID_SNAPSHOT;
			goto prio_error;
		}
		snap_info->prio = prio;
		if (outbead(sock, PRIORITY_OK, struct priority_ok, snap_info->prio) < 0)
			warn("unable to reply to set priority message");
		break;
	prio_error:
		err_len = sizeof(struct priority_error) + strlen(err_msg) + 1;
                if (outhead(sock, PRIORITY_ERROR, err_len) < 0 ||
		    writepipe(sock, &err, sizeof(err)) < 0 ||
		    writepipe(sock, err_msg, err_len - sizeof(struct priority_error)) < 0)
			warn("unable to reply to PRIORITY message with error");
		break;
	}
	case USECOUNT:
	{
		u32 tag = ((struct usecount_info *)message.body)->snap;
		int32_t usecnt_dev = ((struct usecount_info *)message.body)->usecnt_dev;
		int32_t new_usecnt = 0;
		unsigned int err_len;
		uint32_t err = 0;
		char err_msg[MAX_ERRMSG_SIZE];

#ifdef DEBUG_ERROR
		snprintf(err_msg, MAX_ERRMSG_SIZE, "Debug mode to test error messages");
		err_msg[MAX_ERRMSG_SIZE-1] = '\0';
		goto usecnt_error;
#endif

		struct snapshot *snap_info;
		if (tag == (u32)~0UL) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Setting the usecount of the origin.");
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_INVALID_SNAPSHOT;
			goto usecnt_error; /* not really an error though */
		}

		if (!(snap_info = valid_snaptag(sb, tag))) {
			warn("Snapshot tag %u is not valid", tag);
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Snapshot tag %u is not valid", tag);
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_INVALID_SNAPSHOT;
			goto usecnt_error;
		}
		new_usecnt = usecnt_dev + snap_info->usecnt;

		if (((new_usecnt >> 16) != 0) && (usecnt_dev >= 0)) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Usecount overflow.");
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_USECOUNT;
			goto usecnt_error;
		}
		if (((new_usecnt >> 16) != 0) && (usecnt_dev < 0)) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "Usecount underflow.");
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			err = ERROR_USECOUNT;
			goto usecnt_error;
		}
		
		snap_info->usecnt = new_usecnt;
		if (outbead(sock, USECOUNT_OK, struct usecount_ok, .usecount = snap_info->usecnt) < 0)
			warn("unable to reply to USECOUNT message");
		break;

	usecnt_error:
		err_len = sizeof(struct usecount_error) + strlen(err_msg) + 1;
                if (outhead(sock, USECOUNT_ERROR, err_len) < 0 ||
		    writepipe(sock, &err, sizeof(struct usecount_error)) < 0 ||
		    writepipe(sock, err_msg, err_len - sizeof(struct usecount_error)) < 0)
			warn("unable to reply to USECOUNT message with error");
		break;
	}
	case STREAM_CHANGELIST:
	{
		u32 tag1 = ((struct stream_changelist *)message.body)->snap1,
			tag2 = ((struct stream_changelist *)message.body)->snap2;
		char const *err_msg;

#ifdef DEBUG_ERROR
		err_msg = "Debug mode to test error messages";
		goto stream_error;
#endif

		/* check that each snapshot is a valid tag */
		struct snapshot *snapshot1, *snapshot2;
		err_msg = "invalid snapshot tag";
		if (!(snapshot1 = valid_snaptag(sb, tag1))
		    || !(snapshot2 = valid_snaptag(sb, tag2)))
			goto stream_error;

		struct change_list *cl;

		trace_on(printf("generating changelist from snapshot tags %u and %u\n", tag1, tag2););
		err_msg = "unable to generate changelist";
		if ((cl = gen_changelist_tree(sb, snapshot1, snapshot2)) == NULL)
			goto stream_error;

		trace_on(printf("sending stream header\n"););
		if (outbead(sock, STREAM_CHANGELIST_OK, struct changelist_stream, cl->count, 
					sb->snapdata.asi->allocsize_bits) < 0)
			warn("unable to send reply to stream change list message");

		trace_on(printf("streaming "U64FMT" chunk addresses\n", cl->count););
		if (writepipe(sock, cl->chunks, cl->count * sizeof(cl->chunks[0])) < 0)
			warn("unable to send chunks for streaming change list");

		free_change_list(cl);

		break;

	stream_error:
		warn("%s", err_msg);

		int err;
		if ((err = outhead(sock, STREAM_CHANGELIST_ERROR, strlen(err_msg)+1)) < 0 ||
				(err = writepipe(sock, err_msg, strlen(err_msg)+1)) < 0)
			warn("unable to send error for streaming change list message: %s", strerror(-err));
		break;
	}
	case STATUS:
	{
		struct status_request request;
		struct snapshot const *snaplist = sb->image.snaplist;
		unsigned int snapslot;
		char const *err_msg;

#ifdef DEBUG_ERROR
		err_msg = "Debug mode to test error messages";
		goto status_error;
#endif

		if (message.head.length != sizeof(request)) {
			err_msg = "status_request has wrong length";
			goto status_error;
		}

		memcpy(&request, message.body, sizeof(request));

		/* allocate reply */

		/* max_snapbit() + 1 is always >= the number of snapshots,
		 * and will allow the calc_sharing() routine to index the
		 * table by snapbit numbers.
		 * and we need exactly same number of columns as snapshots.
		 * extra rows or columns can be ignored when reducing the
		 * table to be sent because they are always zero.
		 */
		unsigned int num_rows = max_snapbit(snaplist, sb->image.snapshots) + 1;
		unsigned int num_columns = num_rows;
		unsigned int status_count;

		if (request.snap != (u32)~0UL) { /* meaning "all snapshots" */
			status_count = 0;
			for (snapslot = 0; snapslot < sb->image.snapshots; snapslot++)
				if (snaplist[snapslot].tag == request.snap) {
					status_count = 1;
					break;
				}
		} else {
			status_count = sb->image.snapshots;
		}

		unsigned int reply_len = sizeof(struct status_message) +
					status_count * (sizeof(struct status) +
							num_columns * sizeof(uint64_t));
		struct status_message *reply = calloc(reply_len, 1);

		/* calculate the usage statistics */

		uint64_t *share_array = calloc(sizeof(uint64_t) * num_rows * num_columns, 1);

		/* make it look like a two dimensional array so users don't have to
		 * know about the number of columns.  the rows are in ascending
		 * snapbit order.
		 */
		uint64_t **share_table = malloc(sizeof(uint64_t *) * num_rows);
		unsigned int snapbit;
		for (snapbit = 0; snapbit < num_rows; snapbit++)
			share_table[snapbit] = share_array + num_columns * snapbit;

		traverse_tree_chunks(sb, calc_sharing, NULL, share_table);

#ifdef DEBUG_STATUS
		{
			int row, col;

			printf("num_rows=%u num_columns=%u status_count=%u\n", num_rows, num_columns, status_count);

			printf("snapshots:\n");
			for (snapslot = 0; snapslot < sb->image.snapshots; snapslot++)
				printf(" %2u: bit=%d, idtag=%u\n", snapslot, sb->image.snaplist[snapslot].bit, sb->image.snaplist[snapslot].tag);
			printf("\n");

			for (row = 0; row < num_rows; row++) {
				printf("%2d: ", row);
				for (col = 0; col < num_columns; col++)
					printf(" %2llu", share_table[row][col]);
				printf("\n");
			}
			printf("\n");
		}
#endif

		/* fill in reply structure */

		reply->ctime = sb->image.create_time;

		reply->meta.chunksize_bits = sb->metadata.asi->allocsize_bits;
		reply->meta.used = sb->image.meta_chunks_used; 
		reply->meta.free = sb->metadata.asi->freechunks;
	
		reply->store.chunksize_bits = sb->snapdata.asi->allocsize_bits;
		reply->store.used = sb->image.snap_chunks_used; 
		reply->store.free = sb->snapdata.asi->freechunks;

		reply->write_density = 0;

		reply->status_count = status_count;
		reply->num_columns = num_columns;

		unsigned int row, col;
		struct status *snap_status;

		row = 0;
		for (snapslot = 0; snapslot < sb->image.snapshots; snapslot++) {
			if (request.snap != (u32)~0UL && snaplist[snapslot].tag != request.snap)
				continue;

			snap_status = get_snap_status(reply, row);

			snap_status->ctime = snaplist[snapslot].ctime;
			snap_status->snap = snaplist[snapslot].tag;

			for (col = 0; col < num_columns; col++)
				snap_status->chunk_count[col] = share_table[snaplist[snapslot].bit][col];

			row++;
		}

		free(share_table);
		free(share_array);

		/* send it */

		if (outhead(sock, STATUS_OK, reply_len) < 0 ||
			writepipe(sock, reply, reply_len) < 0)
			warn("unable to send status message");

		free(reply);

		break;

	status_error:
		warn("%s", err_msg);
		
		int err;
		if ((err = outhead(sock, STATUS_ERROR, strlen(err_msg)+1)) < 0 ||
				(err = writepipe(sock, err_msg, strlen(err_msg)+1)) < 0)
			warn("unable to send error for status message: %s", strerror(-err));
		break;
	}
	case REQUEST_ORIGIN_SECTORS:
	{
		int err;

		if ((err = outbead(sock, ORIGIN_SECTORS, struct origin_sectors, sb->image.orgsectors)) < 0)
			warn("unable to send origin sectors message: %s", strerror(-err));
		
		break;
	}
	case SHUTDOWN_SERVER:
		return -2;
		
	case PROTOCOL_ERROR: 
	{	struct protocol_error *pe = malloc(message.head.length);
		
		if (!pe) {
			warn("received protocol error message; unable to retreive information");
			break;
		}	
		
		memcpy(pe, message.body, message.head.length);
		
		char * err_msg = "No message sent";
		if (message.head.length - sizeof(struct protocol_error) > 0) {
			pe->msg[message.head.length - sizeof(struct protocol_error) - 1] = '\0';
			err_msg = pe->msg;
		}
		warn("protocol error message - error code: %x unknown code: %x message: %s", 
				pe->err, pe->culprit, err_msg); 
		free(pe);
		break;
	}
	default: 
	{
		uint32_t proto_err  = ERROR_UNKNOWN_MESSAGE;
		char *err_msg = "Server received unknown message";
		warn("snapshot server received unknown message code=%x, length=%u", message.head.code, message.head.length);
		if (outhead(sock, PROTOCOL_ERROR, sizeof(struct protocol_error) + strlen(err_msg) +1) < 0 ||
			writepipe(sock, &proto_err, sizeof(uint32_t)) < 0 ||
			writepipe(sock, &message.head.code, sizeof(uint32_t)) < 0 ||
			writepipe(sock, err_msg, strlen(err_msg) + 1) < 0)
				warn("unable to send unknown message error");
	}

	} /* end switch statement */
	
#ifdef SIMULATE_CRASH
	static int messages = 0;
	if (++messages == 5) {
		warn(">>>>Simulate server crash<<<<");
		exit(1);
	}
#endif
	return 0;

 message_too_long:
	warn("message %x too long (%u bytes) (disconnecting client)", message.head.code, message.head.length);
#ifdef DEBUG_BAD_MESSAGES
	for (;;) {
		unsigned int byte;

		if ((err = readpipe(sock, &byte, 1)))
			return -1;
		warn("%02x", byte);
	}
#endif
	return -1;
 message_too_short:
	warn("message %x too short (%u bytes) (disconnecting client)", message.head.code, message.head.length);
	return -1;
 pipe_error:
	return -1; /* we quietly drop the client if the connect breaks */
}

/* Signal Delivery via pipe */

static int sigpipe;

static void sighandler(int signum)
{
	trace(printf("caught signal %i\n", signum););
	write(sigpipe, (char[]){signum}, 1);
}

static int cleanup(struct superblock *sb)
{
	warn("cleaning up");
	sb->image.flags &= ~SB_BUSY;
	set_sb_dirty(sb);
	save_state(sb);
	return 0;
}

#if 0
static int resolve_host(char *name, int family, void *result, int length)
{
	struct hostent host, *bogus;
	char work[500];
	int err, dumb;

	if ((err = gethostbyname2_r(name, family, &host, work, sizeof(work), &bogus, &dumb))) {
		errno = err;
		return -1;
	}
	memcpy(result, host.h_addr_list[0], host.h_length);
	return host.h_length;
}

static int resolve_self(int family, void *result, int length)
{
	char name[HOST_NAME_MAX + 1];
	if (gethostname(name, HOST_NAME_MAX) == -1)
		return -1;

	return resolve_host(name, family, result, length);
}
#endif

int snap_server_setup(const char *agent_sockname, const char *server_sockname, int *listenfd, int *getsigfd, int *agentfd)
{
	struct sockaddr_un server_addr = { .sun_family = AF_UNIX };
	int server_addr_len = sizeof(server_addr) - sizeof(server_addr.sun_path) + strlen(server_sockname);
	int pipevec[2];

	if (pipe(pipevec) == -1)
		error("Can't create pipe: %s", strerror(errno));
	sigpipe = pipevec[1];
	*getsigfd = pipevec[0];
	
	if (strlen(server_sockname) > sizeof(server_addr.sun_path) - 1) 
		error("server socket name too long, %s", server_sockname);
	strncpy(server_addr.sun_path, server_sockname, sizeof(server_addr.sun_path));
	unlink(server_sockname);

	if ((*listenfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		error("Can't get AF_UNIX socket: %s", strerror(errno));
	if (bind(*listenfd, (struct sockaddr *)&server_addr, server_addr_len) == -1)
		error("Can't bind to socket %s: %s", server_sockname, strerror(errno));
	if (listen(*listenfd, 5) == -1)
		error("Can't listen on socket: %s", strerror(errno));

	warn("ddsnapd server bound to socket %s", server_sockname);

	/* Get agent connection */
	struct sockaddr_un agent_addr = { .sun_family = AF_UNIX };
	int agent_addr_len = sizeof(agent_addr) - sizeof(agent_addr.sun_path) + strlen(agent_sockname);

	trace(warn("Connect to control socket %s", agent_sockname););
	if ((*agentfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		error("Can't get AF_UNIX socket: %s", strerror(errno));
	strncpy(agent_addr.sun_path, agent_sockname, sizeof(agent_addr.sun_path));
	if (agent_sockname[0] == '@')
		agent_addr.sun_path[0] = '\0';
	if (connect(*agentfd, (struct sockaddr *)&agent_addr, agent_addr_len) == -1)
		error("Can't connect to control socket %s: %s", agent_sockname, strerror(errno));
	trace(warn("Established agent control connection"););

	struct server_head server_head = { .type = AF_UNIX, .length = (strlen(server_sockname) + 1) };
	trace(warn("server socket name is %s and length is %d", server_sockname, server_head.length););
	if (writepipe(*agentfd, &(struct head){ SERVER_READY, sizeof(struct server_head) }, sizeof(struct head)) < 0 ||
	    writepipe(*agentfd, &server_head, sizeof(server_head)) < 0 || 
	    writepipe(*agentfd, server_sockname, server_head.length) < 0)
		error("Unable to send SEVER_READY msg to agent: %s", strerror(errno));
	
	return 0;
}

int snap_server(struct superblock *sb, int listenfd, int getsigfd, int agentfd)
{
	unsigned maxclients = 100, clients = 0, others = 3;
	struct client *clientvec[maxclients];
	struct pollfd pollvec[others+maxclients];
	int err = 0; /* FIXME: we never use this */

	pollvec[0] = (struct pollfd){ .fd = listenfd, .events = (POLLIN | POLLHUP | POLLERR) };
	pollvec[1] = (struct pollfd){ .fd = getsigfd, .events = (POLLIN | POLLHUP | POLLERR) };
	pollvec[2] = (struct pollfd){ .fd = agentfd, .events = (POLLIN | POLLHUP | POLLERR) };

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	while (1) {
		trace(warn("Waiting for activity"););

		int activity = poll(pollvec, others+clients, -1);

		if (activity < 0) {
			if (errno != EINTR)
				error("poll failed: %s", strerror(errno));
			continue;
		}

		if (!activity) {
			printf("waiting...\n");
			continue;
		}

		/* New connection? */
		if (pollvec[0].revents) {
			struct sockaddr_in addr;
			unsigned int addr_len = sizeof(addr);
			int clientfd;

			if ((clientfd = accept(listenfd, (struct sockaddr *)&addr, &addr_len))<0)
				error("Cannot accept connection: %s", strerror(errno));

			trace_on(warn("Received connection"););
			assert(clients < maxclients); // !!! send error and disconnect

			struct client *client = malloc(sizeof(struct client));
			*client = (struct client){ .sock = clientfd };
			clientvec[clients] = client;
			pollvec[others+clients] = 
				(struct pollfd){ .fd = clientfd, .events = (POLLIN | POLLHUP | POLLERR) };
			clients++;
		}

		/* Signal? */
		if (pollvec[1].revents) {
			u8 sig = 0;
			/* it's stupid but this read also gets interrupted, so... */
			do { } while (read(getsigfd, &sig, 1) == -1 && errno == EINTR);
			trace_on(warn("Cleaning up before server dies. Caught signal %i", sig););
			cleanup(sb); // !!! don't do it on segfault
			if (sig == SIGINT || sig == SIGTERM) {
				flush_buffers();
				evict_buffers();
				signal(SIGINT, SIG_DFL);
				kill(getpid(), sig); /* commit harikiri */ /* FIXME: use raise()? */
			}
			goto done;
		}

		/* Agent message? */
		if (pollvec[2].revents)
			incoming(sb, &(struct client){ .sock = agentfd, .id = -2, .snap = -2 });

		/* Client message? */
		unsigned i = 0;
		while (i < clients) {
			if (pollvec[others+i].revents) { // !!! check for poll error
				struct client *client = clientvec[i];
				int result;

				trace_off(printf("event on socket %i = %x\n", client->sock, pollvec[others+i].revents););
				if ((result = incoming(sb, client)) == -1) {
					warn("Client %llu disconnected", client->id);

					if (client->flags == USING) {
						if (client->snap != -1) {
							struct snapshot *snap_info;
							if (!(snap_info = valid_snapnum(sb, client->snap))) {
								u32 tag = snapnum_tag(sb, client->snap);
								warn("Snapshot tag %u is not valid", tag);
								goto close_client;
							}
							u32 new_usecnt = snap_info->usecnt - 1;
							if (new_usecnt < 0) {
								warn("Usecount underflow.");
								new_usecnt = 0;
							}
							snap_info->usecnt = new_usecnt;
						}
					}

				close_client:
					save_state(sb); // !!! just for now
					close(client->sock);
					free(client);
					--clients;
					clientvec[i] = clientvec[clients];
					pollvec[others + i] = pollvec[others + clients];
					continue;
				}

				if (result == -2) { // !!! wrong !!!
					cleanup(sb);
					goto done;
				}
			}
			i++;
		}
	}
done:
	// in a perfect world we'd close all the connections
	close(listenfd);
	return err;
}

u32 strtobytes(char const *string)
{
	long bytes = 0;
	char *letter = NULL;

	bytes = strtol(string, &letter, 10);

	if (bytes <= 0)
		return INPUT_ERROR;

	if (letter[0] == '\0')
		return bytes;

	if (letter[1] != '\0')
		return INPUT_ERROR;

	switch (letter[0]) {
	case 'k': case 'K':
		return bytes * (1 << 10);
	case 'm': case 'M':
		return bytes * (1 << 20);
	case 'g': case 'G':
		return bytes * (1 << 30);
	default:
		return INPUT_ERROR;
	}
}

u32 strtobits(char const *string)
{
	long amount = 0;
	u32 bits = 0;
	char* letter = NULL;

	amount = strtol(string, &letter, 10);

	if ((amount <= 0) || (amount & (amount - 1)))
		return INPUT_ERROR;

	while (amount > 1) {
		bits += 1;
		amount >>= 1;
	}

	switch (letter[0]) {
	case '\0':
		break;
	case 'k': case 'K':
		bits += 10;
		break;
	case 'm': case 'M':
		bits += 20;
		break;
	case 'g': case 'G':
		bits += 30;
		break;
	default:
		return INPUT_ERROR;
		break;
	}

	if (letter[1] != '\0')
		return INPUT_ERROR;

	return bits;
}