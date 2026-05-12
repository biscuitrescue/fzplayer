#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} SongList;

bool is_audio(const char *path);
void push_song(SongList *songs, const char *path);
void scan_dir(SongList *songs, const char *path);
void get_songs(SongList *songs);
int cmp_songs(const void *a, const void *b);

#endif
