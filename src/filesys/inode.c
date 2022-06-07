#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"

/** Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK_NUM 124
#define INDIRECT_BLOCK_NUM (BLOCK_SECTOR_SIZE/4)
#define MIN(x, y) ((x)<(y)?(x):(y))


/** On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    bool dir;                               /**< True if is directory */
    block_sector_t doubly_indirect_block;   /**< Doubly indirect block. */
    block_sector_t 
    direct_blocks[DIRECT_BLOCK_NUM];        /**< Direct blocks. */
    off_t length;                           /**< File size in bytes. */
    unsigned magic;                         /**< Magic number. */
  };

static inline size_t bytes_to_sectors(off_t);
static block_sector_t byte_to_sector (const struct inode*, off_t, bool);

static bool inode_allocate (struct inode_disk*, off_t, bool);
static bool inode_extend (struct inode_disk*, off_t);
static bool inode_recur_extend (block_sector_t*, size_t, int);
static void inode_free (struct inode_disk*);
static void inode_recur_free (block_sector_t*, size_t, int);

/** In-memory inode. */
struct inode 
  {
    struct list_elem elem;          /**< Element in inode list. */
    block_sector_t sector;          /**< Sector number of disk location. */
    int open_cnt;                   /**< Number of openers. */
    bool removed;                   /**< True if deleted, false otherwise. */
    int deny_write_cnt;             /**< 0: writes ok, >0: deny writes. */
    int read_length;                /**< Current length visible to read */
    struct inode_disk data;         /**< Inode content. */
    struct lock lock;               /**< Lock for extension */
  };

/** List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/** Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/** Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      if (inode_allocate (disk_inode, length, dir))
        {
          filesys_cache_write (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

/** Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock);
  filesys_cache_read (inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  inode->read_length = inode->data.length;
  return inode;
}

/** Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/** Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/** Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          inode_free (&inode->data);
        }

      free (inode); 
    }
}

/** Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/** Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, true);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode->read_length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = MIN(inode_left, sector_left);

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = MIN(size, min_left);
      if (chunk_size <= 0)
        break;

      filesys_cache_read (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/** Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  bool flag = false;
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  /* Position exceeds EOF */
  if (byte_to_sector (inode, size + offset - 1, true) == (block_sector_t) -1)
  {
    flag = true;
    /* Locks for directories are processed in directory.c */
    if (!inode_is_dir (inode)) lock_acquire (&inode->lock);
    if (!inode_extend (&inode->data, offset + size))
    {
      if (!inode_is_dir (inode)) lock_release (&inode->lock);
      return 0;
    }
    inode->data.length = size + offset;
    filesys_cache_write (inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;
      if (flag) min_left = sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      filesys_cache_write (sector_idx, buffer+bytes_written, 
                           sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  if (flag)
  {
    inode->read_length = inode->data.length;
    if (!inode_is_dir (inode)) lock_release (&inode->lock);
  }

  return bytes_written;
}

/** Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/** Allocate space for inode */
static bool 
inode_allocate (struct inode_disk *disk_inode, off_t length, bool dir)
{
  disk_inode->dir = dir;
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;
  return inode_extend (disk_inode, length);
}

/**
 * Extend the length of the file represented by DISK_INODE by LENGTH. 
 * LENGTH must be positive or zero.
 */
static bool inode_extend (struct inode_disk *disk_inode, off_t length)
{
  static char zeros[BLOCK_SECTOR_SIZE] UNUSED;
  if (length < 0)
    return false;
    
  size_t sectors = bytes_to_sectors (length);
  size_t sector_num = MIN (sectors, DIRECT_BLOCK_NUM);

  /* Use direct blocks */
  for (size_t i=0;i<sector_num;i++)
    if (!inode_recur_extend (disk_inode->direct_blocks+i, 1, 0))
      return false;
  sectors -= sector_num;
  if (sectors == 0) return true;

  /* We need to use doubly indirect blocks */
  return inode_recur_extend (&disk_inode->doubly_indirect_block, sectors, 2);
}

static 
bool inode_recur_extend (block_sector_t *sector, size_t sector_num, int k)
{
  static char zeros[BLOCK_SECTOR_SIZE] = {0};
  if (*sector == 0)
  {
    if (!free_map_allocate (1, sector))
      return false;
    filesys_cache_write (*sector, zeros, 0, BLOCK_SECTOR_SIZE);
  }
  if (k == 0) return true;
  
  size_t sector_per_entry = 1;
  for (int i=1;i<k;i++) sector_per_entry *= INDIRECT_BLOCK_NUM;
  size_t entry_num = DIV_ROUND_UP (sector_num, sector_per_entry);
  ASSERT (entry_num <= INDIRECT_BLOCK_NUM);  

  block_sector_t indirect_block[INDIRECT_BLOCK_NUM];
  filesys_cache_read (*sector, indirect_block, 0, BLOCK_SECTOR_SIZE);

  for (size_t i = 0 ;i < entry_num; i++)
  {
    size_t actual_per_entry = MIN (sector_per_entry, sector_num);
    if (!inode_recur_extend (indirect_block + i, actual_per_entry, k - 1))
      return false;
    sector_num -= actual_per_entry;
  }
  ASSERT (sector_num == 0);
  filesys_cache_write (*sector, indirect_block, 0, BLOCK_SECTOR_SIZE);
  return true;
}

/**
 * Free the sectors occupied by DISK_INODE.
 */
static void inode_free (struct inode_disk *disk_inode)
{
  ASSERT (disk_inode->length >= 0);
  size_t sectors = bytes_to_sectors (disk_inode->length);
  size_t sector_num = MIN (sectors, DIRECT_BLOCK_NUM);
  
  /* Free direct blocks */
  for (size_t i=0;i<sector_num;i++)
    inode_recur_free (disk_inode->direct_blocks+i, 1, 0);
  sectors -= sector_num;
  /* We also need to free doubly indirect blocks */
  if (sectors != 0)
    inode_recur_free (&disk_inode->doubly_indirect_block, sectors, 2);
}

/**
 * Recursively free the entries in an inode.
 * K is the number of level of directed list SECTOR represents.
 */
static void 
inode_recur_free (block_sector_t *sector, size_t sector_num, int k)
{
  ASSERT (*sector != 0);
  if (k == 0)
    free_map_release (*sector, 1);
  else
  {
    size_t sector_per_entry = 1;
    for (int i=1;i<k;i++) sector_per_entry *= INDIRECT_BLOCK_NUM;
    size_t entry_num = DIV_ROUND_UP (sector_num, sector_per_entry);
    ASSERT (entry_num <= INDIRECT_BLOCK_NUM);  

    /* Read from disk the indirect block */
    block_sector_t indirect_block[INDIRECT_BLOCK_NUM];
    filesys_cache_read (*sector, indirect_block, 0, BLOCK_SECTOR_SIZE);

    /* Free each entry recursively */
    for (size_t i = 0 ;i < entry_num; i++)
    {
      size_t actual_per_entry = MIN (sector_per_entry, sector_num);
      inode_recur_free (indirect_block + i, actual_per_entry, k - 1);
      sector_num -= actual_per_entry;
    }
    ASSERT (sector_num == 0);
    free_map_release (*sector, 1);
  }
}

/** Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/** Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/** Returns if INODE is a directory. */
bool inode_is_dir (const struct inode *inode)
{
  return inode->data.dir;
}

/** Returns if INODE has been removed. */
bool inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}

/** API for the lock in struct inode */

void inode_lock_acquire (struct inode *inode)
{
  lock_acquire (&inode->lock);
}
void inode_lock_release (struct inode *inode)
{
  lock_release (&inode->lock);
}

/** Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/** Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos, bool flag) 
{
  ASSERT (inode != NULL);
  if (flag && pos >= inode_length (inode)) return -1;
  size_t sector_pos = pos / BLOCK_SECTOR_SIZE;
  if (sector_pos < DIRECT_BLOCK_NUM)
    return inode->data.direct_blocks[sector_pos];
  sector_pos -= DIRECT_BLOCK_NUM;

  /** The sector entry is in doubly indirect blocks. */
  block_sector_t indirect_block[INDIRECT_BLOCK_NUM];
  filesys_cache_read (inode->data.doubly_indirect_block, indirect_block,
                      0, BLOCK_SECTOR_SIZE);
  filesys_cache_read (indirect_block [sector_pos / INDIRECT_BLOCK_NUM], 
                      indirect_block, 0, BLOCK_SECTOR_SIZE);
  return indirect_block [sector_pos % INDIRECT_BLOCK_NUM];
}