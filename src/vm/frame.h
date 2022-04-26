#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"

struct frame{
    void *frame;
    void *page;
    tid_t tid;
    struct list_elem elem;
};

void vm_init (void);
void vm_add_fe (void*);