#ifndef FILESYS_FSUTIL_H
#define FILESYS_FSUTIL_H

void fsutil_ls (char **argv);
void fsutil_cat (char **argv);
void fsutil_rm (char **argv);
void fsutil_extract (char **argv);
void fsutil_append (char **argv);
void fsutil_parse_path (const char *, char *, char *);

#endif /**< filesys/fsutil.h */
