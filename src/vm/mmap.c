#include <hash.h>
#include "filesys/file.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "vm/mmap.h"
#include "vm/page.h"

static bool check_page_validity (const void*, unsigned);
static mapid_t insert_mmap_file (struct file*, void*);
static void hash_unmap_file (struct hash_elem*, void*);

/* hash functions required by hash table implementation */
unsigned hash_mmap_file (const struct hash_elem *e, void *aux UNUSED)
{
    struct mmap_file *pe;
    pe = hash_entry (e, struct mmap_file, elem);    
    return hash_int ((int) pe->mapid);
}
bool hash_less_mmap_file (const struct hash_elem *a,
                       const struct hash_elem *b, void *aux UNUSED)
{
    return hash_entry (a, struct mmap_file, elem)->mapid
         < hash_entry (b, struct mmap_file, elem)->mapid;
}
void hash_free_mmap_file (struct hash_elem *e, void *aux UNUSED)
{
    free (hash_entry (e, struct mmap_file, elem));
}

/**
 * Map a file FILE to address ADDR 
 */
mapid_t map_file (struct file *file, void *addr)
{
    lock_acquire (&file_lock);
    file = file_reopen (file);
    /* Reopen unsuccessful */
    if (file == NULL)
    {
        lock_release (&file_lock);
        return MMAP_ERROR;
    }
    off_t read_bytes = file_length (file);
    lock_release (&file_lock);
    if (!check_page_validity (addr, read_bytes))
        return MMAP_ERROR;
    struct hash* spt = &process_current ()->spl_page_table;
    for (int idx = 0; idx * PGSIZE < read_bytes; idx++)
    {
        size_t page_read_bytes = (read_bytes - idx * PGSIZE) < PGSIZE 
                                 ? (read_bytes - idx * PGSIZE) : PGSIZE;
        add_spl_pe (PG_MMAP, spt, file, idx * PGSIZE, 
                    addr + idx * PGSIZE, page_read_bytes, 
                    PGSIZE - page_read_bytes, true);
    }
    /* add mmap file entry into mmap_table */
    return insert_mmap_file (file, addr);
}

/**
 * Unmap a given file 
 */
void unmap_file (struct mmap_file *file)
{
    struct hash *mmap_table = &process_current ()->mmap_table;
    hash_unmap_file (&file->elem, NULL);
    ASSERT (hash_delete (mmap_table, &file->elem) != NULL)
    free (file);
}

/**
 * Unmap the whole mmap table
 * NOTE: this function does not change the table 
 */
void unmap_mmap_table (struct hash *mmap_table)
{
    hash_apply (mmap_table, hash_unmap_file);
}

/**
 * Find the mmap file entry with the mmap id
 */
struct mmap_file* find_mmap_file (mapid_t mapid)
{
    struct mmap_file f;
    f.mapid = mapid;
    struct hash_elem *e = hash_find (&process_current ()->mmap_table, &f.elem);
    return hash_entry (e, struct mmap_file, elem);
}

/**
 * The hash function for unmapping a file.
 * NOTE: this function does not change the hash list.
 */
static void hash_unmap_file (struct hash_elem *e, void *aux UNUSED)
{
    enum intr_level old_level = intr_enable ();
    struct mmap_file *file = hash_entry (e, struct mmap_file, elem);
    lock_acquire (&file_lock);
    off_t file_len = file_length (file->file);
    file_seek (file->file, 0);
    lock_release (&file_lock);
    
    void* upage = file->addr;
    off_t read_bytes = file_len;
    uint32_t *pd = thread_current ()->pagedir;
    
    /* write memory to file */
    while (read_bytes > 0) 
    {
        off_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        
        /* Write back the memory content */
        page_unmap (pd, upage);
        
        /* Advance. */
        read_bytes -= page_read_bytes;
        upage += PGSIZE;
    }

    lock_acquire (&file_lock);
    file_close (file->file);
    lock_release (&file_lock);

    intr_set_level (old_level);
}

/**
 * @brief 
 * Insert a mmap file onto the process mmap table.
 * @param file the file pointer
 * @param addr start address
 * @return mapid_t
 * the newly generated mmap id
 */
static mapid_t insert_mmap_file (struct file *file, void *addr)
{
    struct hash *mmap_table = &process_current ()->mmap_table;
    struct mmap_file *f = malloc (sizeof (struct mmap_file));
    f->file = file;
    f->mapid = process_current ()->next_id++;
    f->addr = addr;
    if (hash_insert (mmap_table, &f->elem) != NULL)
        return MMAP_ERROR;
    return f->mapid;
}

/**
 * Verify that the pages are not mapped
 * @return true if page is valid
 */
static bool check_page_validity (const void *buffer, unsigned size)
{
  ASSERT (pg_ofs (buffer) == 0);
  struct hash *spt = &process_current ()->spl_page_table;
  for (unsigned tmp = 0; tmp <= (size - 1) / PGSIZE; tmp++)
    if (find_spl_pe (spt, buffer + tmp * PGSIZE)) 
        return false;
  return true;
}