#include <dirent.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} SongList;

bool is_audio(const char *path);
void push_song(SongList *songs, const char *path);
void scan_dir(SongList *songs, const char *path);

void scan_dir(SongList *songs, const char *path) {
  DIR *dir = opendir(path);

  if (dir == NULL) {
    perror("Couldn't open directory");
    exit(EXIT_FAILURE);
  }
  
  struct dirent *entry;
  char full_path[PATH_MAX];
  
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    
    snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
    
    if (entry->d_type == DT_DIR) {
      scan_dir(songs, full_path);
    } else if (is_audio(entry->d_name)) {
      push_song(songs, full_path);
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
  const char *ext = (strrchr(path, '.'));

  if (!ext || ext[1] == '\0') {
    return false;
  }
  ext++;
  return !strcasecmp(ext, "mp3") || !strcasecmp(ext, "flac") ||
         !strcasecmp(ext, "ogg") || !strcasecmp(ext, "wav");
}

void get_songs(SongList *songs) {
  if (songs->len == 0) {
    printf("Nothing in directory, exiting...");
    exit(EXIT_SUCCESS);
  }
  for (size_t i = 0; i < songs->len; i++) {
    printf("%s\n", songs->items[i]);
    /* printf("%zu\n", i); */
  }
}

int main(int argc, char *argv[]) {
  char buf[PATH_MAX];
  char *path;

  if (argc < 2) {
    if (getcwd(buf, sizeof(buf)) == NULL) {
      perror("Couldn't set cwd");
      exit(EXIT_FAILURE);
    }
    path = buf;
  } else {
    path = argv[1];
  }

  SongList songs;
  songs.cap = 16;
  songs.len = 0;

  songs.items = malloc(sizeof(char *) * songs.cap);

  scan_dir(&songs, path);
  get_songs(&songs);

  for (size_t i = 0; i < songs.len; i++) {
    free(songs.items[i]);
  }

  free(songs.items);

  return 0;
}
