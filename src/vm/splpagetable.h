#ifndef __SPL_PT_H
#define __SPL_PT_H
#include <hash.h>
#include "filesys/off_t.h"

enum page_type {
    PG_FILE,
    PG_ZERO,
    PG_MISC,
    PG_SWAP
};

/* supplementary page entry, or SPE */
struct spl_pe{
    enum page_type type;    /* type of this page */
    struct file *file;      /* the file where the page is stored */
    off_t offset;           /* file offset */
    uint8_t *upage;         /* user page */
    uint8_t *kpage;         /* physical frame */
    uint32_t read_bytes;    /* bytes to be read */
    uint32_t zero_bytes;    /* bytes to be set to zero */
    size_t slot;            /* swap slot this page is in */
    bool writable;          /* is writable */
    bool present;           /* is present in physical memory */
    struct hash_elem elem;  /* hash elem */
};

unsigned hash_spl_pe (const struct hash_elem*, void*);
bool hash_less_spl_pe (const struct hash_elem*,
                       const struct hash_elem*, void*);
void hash_free_spl_pe (struct hash_elem*, void*);

bool load_page (uint8_t*);
bool add_spl_pe (enum page_type, struct hash*, struct file*, 
                 off_t, uint8_t*, uint32_t, uint32_t, bool);
bool is_writable (const void*);
void free_spl_pt (struct hash *);

#endif