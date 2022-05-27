#ifndef __MMAP_H
#define __MMAP_H
#include <hash.h>
#include "filesys/file.h"
#include "userprog/syscall.h"

#define MMAP_ERROR -1
typedef int mapid_t;

struct mmap_file {
    struct file *file;      /**< pointer to the file. */
    struct hash_elem elem;  /**< hash element. */
    mapid_t mapid;          /**< map id for this entry */
    void *addr;             /**< mapping address */
};
unsigned hash_mmap_file (const struct hash_elem*, void*);
bool hash_less_mmap_file (const struct hash_elem*, const struct hash_elem*, void*);
void hash_free_mmap_file (struct hash_elem*, void*);

struct mmap_file* find_mmap_file (mapid_t);
mapid_t map_file (struct file*, void*);
void unmap_file (struct mmap_file*);
void unmap_mmap_table (struct hash*);
#endif