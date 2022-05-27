#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#define SECTOR_PER_PAGE (4096 / BLOCK_SECTOR_SIZE)

static struct block *swap_device;
static struct bitmap *map;
static struct lock swap_lock;

void swap_init (void)
{
    swap_device = block_get_role (BLOCK_SWAP);
    if (swap_device == NULL)
        PANIC ("swap_init: no swap device found");
    map = bitmap_create (block_size (swap_device) / SECTOR_PER_PAGE);
    if (map == NULL)
        PANIC ("swap_init: bitmap creation failed");
    lock_init (&swap_lock);
}

/**
 * Swap out the frame located at KPAGE
 * returns the slot number assigned in the swap device
 * if failed, return BITMAP_ERROR
 */
size_t swap_out (void *kpage)
{
    ASSERT (pg_ofs (kpage) == 0);
    lock_acquire (&swap_lock);
    size_t slot = bitmap_scan_and_flip (map, 0, 1, false);
    if (slot == BITMAP_ERROR)
    {
        printf("errored");
        lock_release (&swap_lock);
        return BITMAP_ERROR;
    }
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_write (swap_device, slot * SECTOR_PER_PAGE + i,
                     kpage + i * BLOCK_SECTOR_SIZE);
    lock_release (&swap_lock);
    return slot;
}

/**
 * Swap in a page from swap device at given SLOT to KPAGE
 */
void swap_in (size_t slot, void *kpage)
{
    lock_acquire (&swap_lock);
    // Assert that the given slot is not empty
    ASSERT (bitmap_test (map, slot));
    ASSERT (pg_ofs (kpage) == 0);
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_read (swap_device, slot * SECTOR_PER_PAGE + i,
                    kpage + i * BLOCK_SECTOR_SIZE);
    bitmap_flip (map, slot);
    lock_release (&swap_lock);
}
