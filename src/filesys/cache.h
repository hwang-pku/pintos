#ifndef __FILESYS_CACHE_H
#define __FILESYS_CACHE_H
#include <devices/block.h>

#define CACHE_SIZE 64

void filesys_cache_init (void);
void filesys_cache_read (block_sector_t, void*);
void filesys_cache_write (block_sector_t, const void*);
void filesys_cache_close (void);

#endif