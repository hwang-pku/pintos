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
    struct lock lock;
};

static struct FCE fct[CACHE_SIZE];

static struct FCE* filesys_load_cache (block_sector_t);
static struct FCE* filesys_get_cache (void);
static void filesys_cache_flush (struct FCE*);
static struct FCE* filesys_find_fce (block_sector_t);

void filesys_cache_init (void)
{
    for (int i=0;i<CACHE_SIZE;i++)
    {
        fct[i].available = true;
        lock_init (&fct[i].lock);
    }
}

void filesys_cache_read (block_sector_t id, void *buffer)
{
    struct FCE *fce = filesys_load_cache (id);
    memcpy (buffer, fce->cache, BLOCK_SECTOR_SIZE);
    lock_release (&fce->lock);
}

void filesys_cache_write (block_sector_t id, const void *buffer)
{
    struct FCE *fce = filesys_load_cache (id);
    memcpy (fce->cache, buffer, BLOCK_SECTOR_SIZE);
    fce->dirty = true;
    lock_release (&fce->lock);
}

void filesys_cache_close ()
{
    for (int i=0;i<CACHE_SIZE;i++)
        if (!fct[i].available)
        {
            lock_acquire (&fct[i].lock);
            filesys_cache_flush (fct+i);
            lock_release (&fct[i].lock);
        }
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
    static int clock = 0;
    while (true)
    {
        lock_acquire (&fct[clock].lock);
        if (fct[clock].available || !fct[clock].accessed)
            break;
        fct[clock].accessed = false;
        lock_release (&fct[clock].lock);
        clock = (clock+1)%CACHE_SIZE;
    }
    if (!fct[clock].available)
        filesys_cache_flush (fct + clock);
    return fct + clock;
}

static void filesys_cache_flush (struct FCE *fce)
{
    ASSERT (lock_held_by_current_thread (&fce->lock));
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
    {
        lock_acquire (&fct[i].lock);
        if (!fct[i].available && fct[i].sector_id == id)
            return fct+i;
        lock_release (&fct[i].lock);
    }
    return NULL;
}
