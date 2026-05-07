#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} SongList;

void push_song(SongList *songs, const char *path) {
    if (songs->len >= songs->cap) {
        songs->cap *= 2;

        songs->items = realloc(songs->items, sizeof(char *) * songs->cap);
    }

    songs->items[songs->len] = strdup(path);
    songs->len++;
}

bool is_audio(const char *path) {
    char *ext = (strrchr(path, '.')) + 1;
    printf("%s", ext);

    if (!ext)
        return false;

    return !strcasecmp(ext, "mp3") || !strcasecmp(ext, "flac") ||
        !strcasecmp(ext, "ogg") || !strcasecmp(ext, "wav");
}

int main() {
    SongList songs;
    songs.cap = 16;
    songs.len = 0;

    songs.items = malloc(sizeof(char *) * songs.cap);

    for (size_t i = 0; i < songs.len; i++) {
        free(songs.items[i]);
    }

    free(songs.items);

    return 0;
}
