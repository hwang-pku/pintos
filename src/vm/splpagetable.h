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

struct spl_pe{
    enum page_type type;
    struct file *file;
    off_t offset;
    uint8_t *upage;
    uint8_t *kpage;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    size_t slot;
    bool writable;
    bool present;
    struct hash_elem elem;
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