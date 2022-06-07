#include "filesys/cache.h"
#include <stdio.h>
#include <string.h>
#include <kernel/list.h>
#include <devices/block.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

/* Filesys Cache Entry */
struct FCE {
    block_sector_t sector_id;
    uint8_t cache[BLOCK_SECTOR_SIZE];

    bool available;
    bool dirty;
    bool accessed;
};

static struct FCE fct[CACHE_SIZE];
static struct lock fc_lock;

static struct FCE* filesys_load_cache (block_sector_t);
static struct FCE* filesys_get_cache (void);
static void filesys_cache_flush (struct FCE*);
static struct FCE* filesys_find_fce (block_sector_t);

void filesys_cache_init (void)
{
    lock_init (&fc_lock);
    for (int i=0;i<CACHE_SIZE;i++)
        fct[i].available = true;
}

void filesys_cache_read (block_sector_t id, void *buffer)
{
    lock_acquire (&fc_lock); 
    struct FCE *fce = filesys_load_cache (id);
    memcpy (buffer, fce->cache, BLOCK_SECTOR_SIZE);
    lock_release (&fc_lock);
}

void filesys_cache_write (block_sector_t id, const void *buffer)
{
    lock_acquire (&fc_lock);
    struct FCE *fce = filesys_load_cache (id);
    memcpy (fce->cache, buffer, BLOCK_SECTOR_SIZE);
    fce->dirty = true;
    lock_release (&fc_lock);
}

void filesys_cache_close ()
{
    lock_acquire (&fc_lock);
    for (int i=0;i<CACHE_SIZE;i++)
        if (!fct[i].available)
        {
            if (fct[i].sector_id == FREE_MAP_SECTOR)
            {
                for (int j = 0 ;j<BLOCK_SECTOR_SIZE;j++)
                    printf ("%d", fct[i].cache[j]);
                printf("\n");
            }
            filesys_cache_flush (fct+i);
            printf ("%d\n", fct[i].sector_id);
        }
    lock_release (&fc_lock);
    struct inode *inode = inode_open (0);
    printf ("length: %d\n", inode_length (inode));
}

/*
void filesys_cache_evict (block_sector_t id)
{
    struct FCE* fce = filesys_find_fce (id);
    if (fce != NULL)
        filesys_cache_flush (fce);
}

bool filesys_cache_empty (block_sector_t id)
{
    for (int i=0;i<CACHE_SIZE;i++)
        if (!fct[i].available)
            return false;
    return true;
}
*/
static struct FCE* filesys_load_cache (block_sector_t id)
{
    struct FCE* fce = filesys_find_fce (id);
    if (fce == NULL)
    {
        fce = filesys_get_cache ();
        ASSERT (fce != NULL && fce->available);

        block_read (fs_device, id, fce->cache);
        fce->sector_id = id;
        fce->available = false;
        fce->dirty = false;
    }
    fce->accessed = true;
    return fce;
}

static struct FCE* filesys_get_cache (void)
{
    ASSERT (lock_held_by_current_thread (&fc_lock));
    static int clock = 0;
    while (true)
    {
        if (fct[clock].available || !fct[clock].accessed)
            break;
        fct[clock].accessed = false;
        clock = (clock+1)%CACHE_SIZE;
    }
    if (!fct[clock].available)
        filesys_cache_flush (fct + clock);
    return fct + clock;
}

static void filesys_cache_flush (struct FCE *fce)
{
    ASSERT (lock_held_by_current_thread (&fc_lock));
    ASSERT (fce != NULL && !fce->available);
    fce->available = true;
    if (fce->dirty)
    {
        block_write (fs_device, fce->sector_id, fce->cache);
        fce->dirty = false;
    }
}

static struct FCE* filesys_find_fce (block_sector_t id)
{
    for (int i=0;i<CACHE_SIZE;i++)
        if (!fct[i].available && fct[i].sector_id == id)
            return fct+i;
    return NULL;
}
