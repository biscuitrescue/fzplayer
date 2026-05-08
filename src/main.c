#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <linux/limits.h>
#include <dirent.h>

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} SongList;

bool is_audio(const char *path);

void read_library(SongList *songs, const char *cwd) {

  DIR *dir;
  struct dirent *entry;
  dir = opendir(cwd);

  if (dir == NULL) {
    perror("Couldn't open directory");
    exit(EXIT_FAILURE);
  }

  while ((entry = readdir(dir)) != NULL) {
    if (is_audio(entry->d_name)) {
      printf("%s\n", entry->d_name);
    }
  }

  closedir(dir);
}

void push_song(SongList *songs, const char *path) {
  if (songs->len >= songs->cap) {
    songs->cap *= 2;

    songs->items = realloc(songs->items, sizeof(char *) * songs->cap);
  }

  songs->items[songs->len] = strdup(path);
  songs->len++;
}

bool is_audio(const char *path) {
  char *ext = (strrchr(path, '.'));

  if (!ext || ext[1] == '\0') {
    return false;
  }

  ext++;
  
  return !strcasecmp(ext, "mp3") || !strcasecmp(ext, "flac") ||
         !strcasecmp(ext, "ogg") || !strcasecmp(ext, "wav");
}

int main(int argc, char *argv[]) {
  char cwd[PATH_MAX];

  if (argc == 0) {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      perror("Couldn't set cwd");
      exit(EXIT_FAILURE);
    }
  } else {
    const char *cwd = ".";
  }

  SongList songs;
  songs.cap = 16;
  songs.len = 0;

  songs.items = malloc(sizeof(char *) * songs.cap);

  read_library(&songs, cwd);

  for (size_t i = 0; i < songs.len; i++) {
    free(songs.items[i]);
  }

  free(songs.items);

  return 0;
}
