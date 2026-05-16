#include <linux/limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "library.h"

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
  qsort(songs.items, songs.len, sizeof(char *), cmp_songs);
  get_songs(&songs);

  for (size_t i = 0; i < songs.len; i++) {
    free(songs.items[i]);
  }

  free(songs.items);

  return 0;
}
