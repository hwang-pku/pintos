#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/process.h"

/** A directory. */
struct dir 
  {
    struct inode *inode;                /**< Backing store. */
    off_t pos;                          /**< Current position. */
  };

/** A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /**< Sector number of header. */
    char name[NAME_MAX + 1];            /**< Null terminated file name. */
    bool in_use;                        /**< In use or free? */
  };

static bool dir_add_parent (struct dir*, struct dir*);

/** Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. 
   Open a bonus entry slot for parent entry. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt*(sizeof (struct dir_entry)+1), true);
}

/** Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = sizeof(struct dir_entry);
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/** Opens and returns the directory for the given PATH.
    Returns a null pointer on failure. */
struct dir *dir_open_path (const char *path)
{
  size_t len = strlen(path);
  char tpath[len + 1];
  strlcpy (tpath, path, len+1);
  struct dir *cwd = NULL, *nxt;
  struct process *p = process_current ();
  if ((len != 0&&path[0] == '/') || p == NULL || p->cwd == NULL)
    cwd = dir_open_root ();
  else
    cwd = dir_reopen (p->cwd);
  if (len == 0) return cwd;
  char *save_ptr;
  for (char *token = strtok_r (tpath, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
  {
    struct inode *inode;
    bool flag = dir_lookup(cwd,token,&inode)&&((nxt=dir_open(inode))!=NULL);
    dir_close (cwd);
    if (!flag) return false;
    cwd = nxt;
  }
  /* Return NULL if dir does not exist any more */
  if (inode_is_removed (dir_get_inode (cwd)))
  {
    dir_close (cwd);
    return NULL;
  }

  return cwd;
}


/** Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/** Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/** Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/** Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (const struct dir *dir) 
{
  return dir->inode;
}

/** Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/** Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. 
   This function also supports "." and ".." */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  inode_lock_acquire (dir_get_inode (dir));
  if (!strcmp (name, "."))
    *inode = inode_reopen (dir->inode);
  else if (!strcmp (name, ".."))
    *inode = dir_get_parent (dir);
  else if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;
  inode_lock_release (dir_get_inode (dir));

  return *inode != NULL;
}

/** Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, 
         block_sector_t inode_sector, bool is_dir)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);


  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  inode_lock_acquire (dir_get_inode (dir));

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Check that dir is not removed */
  if (dir_is_removed (dir))
    goto done;

  /* If the inode added is a directory */
  if (is_dir)
  {
    struct dir* child = dir_open (inode_open (inode_sector));
    bool flag = dir_add_parent (child, dir);
    free (child);
    if (!flag) goto done;
  }

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = sizeof e; inode_read_at (dir->inode,&e,sizeof e,ofs)==sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  inode_lock_release (dir_get_inode (dir));
  return success;
}

/** Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  inode_lock_acquire (dir_get_inode (dir));
  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Avoid removing non-empty dir. */
  if (inode_is_dir (inode))
  {
    struct dir *dir = dir_open (inode);
    bool is_empty = dir_is_empty (dir);
    free (dir);
    if (!is_empty) goto done;
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  inode_lock_release (dir_get_inode (dir));
  return success;
}

/** Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  inode_lock_acquire (dir_get_inode (dir));
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          inode_lock_release (dir_get_inode (dir));
          return true;
        } 
    }
  inode_lock_release (dir_get_inode (dir));
  return false;
}

/**
 * Get the parent directory of a directory DIR.
 */
struct inode* dir_get_parent (const struct dir *dir)
{
  struct dir_entry e;
  if (inode_get_inumber (dir->inode) == ROOT_DIR_SECTOR)
    return inode_reopen (dir->inode);
  
  /* If dir already removed */
  if (inode_is_removed (dir_get_inode (dir)))
    return NULL;
  
  ASSERT (inode_read_at (dir->inode, &e, sizeof e, 0) == sizeof e);
  return inode_open (e.inode_sector);
}

/**
 * Checks whether a dir is empty.
 * Returns true iff dir is empty.
 */
bool dir_is_empty (const struct dir *dir)
{
  struct dir_entry e;
  for (off_t ofs = sizeof e; 
       inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e; 
       ofs += sizeof e) 
    if (e.in_use)
      return false;
  return true;
}

/**
 * Check if a dir has already been removed.
 */
bool dir_is_removed (struct dir *dir)
{
  return inode_is_removed (dir_get_inode (dir));
}

/**
 * Add a parent directory PARENT to directory DIR.
 */
static bool dir_add_parent (struct dir *dir, struct dir *parent)
{
  struct dir_entry e;
  e.in_use = true;
  e.inode_sector = inode_get_inumber(parent->inode);
  return inode_write_at (dir->inode, &e, sizeof e, 0) == sizeof e;
}