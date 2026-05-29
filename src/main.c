#include <linux/limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "library.h"
#include "player.h"

int main(int argc, char *argv[]) {
  char buf[PATH_MAX];
  char *path;

  char abs_path[PATH_MAX];
  if (argc < 2) {
    if (getcwd(buf, sizeof(buf)) == NULL) {
      perror("Couldn't set cwd");
      exit(EXIT_FAILURE);
    }
    path = buf;
  } else {
    path = argv[1];
  }

  if (realpath(path, abs_path) == NULL) {
    perror("Couldn't resolve absolute path");
    exit(EXIT_FAILURE);
  }
  path = abs_path;

  SongList songs;
  songs.cap = 16;
  songs.len = 0;

  songs.items = malloc(sizeof(char *) * songs.cap);

  scan_dir(&songs, path);
  qsort(songs.items, songs.len, sizeof(char *), cmp_songs);
  start_player(&songs);

  for (size_t i = 0; i < songs.len; i++) {
    free(songs.items[i]);
  }

  free(songs.items);

  return 0;
}
