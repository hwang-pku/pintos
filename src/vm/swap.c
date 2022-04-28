#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/synch.h"
#define SECTOR_PER_PAGE 4096 / BLOCK_SECTOR_SIZE

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
 * Swap out the page located at UPAGE
 * returns the slot number assigned in the swap device
 * if failed, return BITMAP_ERROR
 */
size_t swap_out (void *upage)
{
    lock_acquire (&swap_lock);
    size_t slot = bitmap_scan_and_flip (swap_device, 0, 1, false);
    if (slot == BITMAP_ERROR)
    {
        lock_release (&swap_lock);
        return BITMAP_ERROR;
    }
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_write (swap_device, slot * SECTOR_PER_PAGE + i,
                     upage + i * BLOCK_SECTOR_SIZE);
    lock_release (&swap_lock);
    return slot;
}

/**
 * Swap in a page from swap device at given SLOT to UPAGE
 */
void swap_in (size_t slot, void *upage)
{
    // Assert that the given slot is not empty
    ASSERT (bitmap_test (map, slot));
    lock_acquire (&swap_lock);
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
        block_read (swap_device, slot * SECTOR_PER_PAGE + i,
                    upage + i * BLOCK_SECTOR_SIZE);
    bitmap_flip (map, slot);
    lock_release (&swap_lock);
}
