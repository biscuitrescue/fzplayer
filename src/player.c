#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "player.h"

typedef struct {
  size_t song_index;
  int score;
} MatchResult;

static struct {
  char query[256];
  int query_len;

  MatchResult *matches;
  size_t matches_count;
  size_t selected_match_index;
  size_t scroll_offset;

  int mpv_sock;
  pid_t mpv_pid;
  char mpv_socket_path[PATH_MAX];

  double time_pos;
  double duration;
  double volume;
  bool is_paused;

  char current_song_path[PATH_MAX];
  char current_song_title[256];
  int active_song_index;

  bool running;
  unsigned long frame_count;

  struct termios orig_termios;

  bool vim_mode;
  char notification[128];
  time_t notification_expiry;
} state;

static void disable_raw_mode(void);
static void enable_raw_mode(void);
static void cleanup_player(void);
static void handle_signal(int sig);
static const char *get_basename(const char *path);
static int fuzzy_match(const char *target, const char *query);
static int cmp_matches(const void *a, const void *b);
static void update_matches(SongList *songs);
static void send_mpv_cmd(int sock, const char *cmd);
static void play_song(SongList *songs, size_t index);
static void parse_mpv_message(const char *line, SongList *songs);
static void draw_ui(SongList *songs);
static void show_notification(const char *msg);
static void shuffle_songs(SongList *songs);
static void play_random_song(SongList *songs);

static void disable_raw_mode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.orig_termios);
  // Show cursor, clear screen below cursor, and reset colors
  printf("\033[?25h\033[0m\n");
  fflush(stdout);
}

static void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &state.orig_termios) == -1) {
    perror("tcgetattr failed");
    exit(EXIT_FAILURE);
  }

  struct termios raw = state.orig_termios;
  // Enter raw mode: disable echo, canonical mode, extended processing, Ctrl-S/Q
  // flow control
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_oflag &= ~(
      OPOST); // Disable output processing (all print statements will use \r\n)
  raw.c_cflag |= (CS8);

  raw.c_cc[VMIN] = 0; // Non-blocking read
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    perror("tcsetattr failed");
    exit(EXIT_FAILURE);
  }

  // Hide cursor
  printf("\033[?25l");
  fflush(stdout);
}

// Cleanup function registered at exit
static void cleanup_player(void) {
  disable_raw_mode();

  if (state.mpv_sock >= 0) {
    close(state.mpv_sock);
    state.mpv_sock = -1;
  }

  if (state.mpv_pid > 0) {
    // Gracefully terminate mpv, then force if needed
    kill(state.mpv_pid, SIGTERM);
    waitpid(state.mpv_pid, NULL, WNOHANG);
  }

  if (state.mpv_socket_path[0] != '\0') {
    unlink(state.mpv_socket_path);
  }

  if (state.matches) {
    free(state.matches);
    state.matches = NULL;
  }
}

// Handle termination signals
static void handle_signal(int sig) {
  (void)sig;
  exit(0);
}

// Get the filename from a full path
static const char *get_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Show a temporary status notification message
static void show_notification(const char *msg) {
  strncpy(state.notification, msg, sizeof(state.notification) - 1);
  state.notification[sizeof(state.notification) - 1] = '\0';
  state.notification_expiry = time(NULL) + 2; // Display for 2 seconds
}

// Shuffle songs list using Fisher-Yates algorithm
static void shuffle_songs(SongList *songs) {
  if (songs->len <= 1)
    return;
  for (size_t i = songs->len - 1; i > 0; i--) {
    size_t j = rand() % (i + 1);
    char *tmp = songs->items[i];
    songs->items[i] = songs->items[j];
    songs->items[j] = tmp;
  }
}

// Play a random song from the filtered list (or fallback to full list)
static void play_random_song(SongList *songs) {
  if (state.matches_count > 0) {
    size_t rand_match_idx = rand() % state.matches_count;
    state.selected_match_index = rand_match_idx;
    size_t orig_idx = state.matches[rand_match_idx].song_index;
    play_song(songs, orig_idx);
    char msg[128];
    snprintf(msg, sizeof(msg), "Playing random: %s", state.current_song_title);
    show_notification(msg);
  } else if (songs->len > 0) {
    size_t rand_idx = rand() % songs->len;
    play_song(songs, rand_idx);
    char msg[128];
    snprintf(msg, sizeof(msg), "Playing random: %s", state.current_song_title);
    show_notification(msg);
  }
}

// Fuzzy Matching Algorithm
static int fuzzy_match(const char *target, const char *query) {
  if (!query || *query == '\0') {
    return 0; // Empty query matches all files
  }

  int score = 0;
  const char *t = target;
  const char *q = query;
  int last_idx = -1;
  int consecutive_matches = 0;

  while (*q != '\0') {
    char q_char = *q;
    if (q_char >= 'A' && q_char <= 'Z')
      q_char += 32; // Lowercase

    const char *match = NULL;
    const char *curr = t;

    while (*curr != '\0') {
      char t_char = *curr;
      if (t_char >= 'A' && t_char <= 'Z')
        t_char += 32;
      if (t_char == q_char) {
        match = curr;
        break;
      }
      curr++;
    }

    if (!match) {
      return -1; // Subsequence not matched
    }

    int curr_idx = match - target;

    // Bonus for word beginnings or special delimiters
    if (curr_idx == 0 || target[curr_idx - 1] == ' ' ||
        target[curr_idx - 1] == '/' || target[curr_idx - 1] == '-' ||
        target[curr_idx - 1] == '_' || target[curr_idx - 1] == '.') {
      score += 150;
    }

    // Bonus for consecutive character matches
    if (last_idx != -1 && curr_idx == last_idx + 1) {
      consecutive_matches++;
      score += 80 + consecutive_matches * 25;
    } else {
      consecutive_matches = 0;
    }

    // Distance penalty
    if (last_idx != -1) {
      int dist = curr_idx - last_idx - 1;
      score -= dist * 8;
    }

    last_idx = curr_idx;
    t = match + 1;
    q++;
  }

  // Length penalty (shorter matching names rank higher)
  score -= strlen(target) * 2;

  return score;
}

// Comparer for qsort (sorts matches by score descending)
static int cmp_matches(const void *a, const void *b) {
  int score_a = ((const MatchResult *)a)->score;
  int score_b = ((const MatchResult *)b)->score;
  return score_b - score_a;
}

// Re-filters and sorts the song list based on the search query
static void update_matches(SongList *songs) {
  state.matches_count = 0;

  for (size_t i = 0; i < songs->len; i++) {
    const char *song_name = get_basename(songs->items[i]);
    int score = fuzzy_match(song_name, state.query);

    if (score >= 0) {
      state.matches[state.matches_count].song_index = i;
      state.matches[state.matches_count].score = score;
      state.matches_count++;
    }
  }

  qsort(state.matches, state.matches_count, sizeof(MatchResult), cmp_matches);

  // Reset selection if it falls out of range
  if (state.selected_match_index >= state.matches_count) {
    state.selected_match_index =
        state.matches_count > 0 ? state.matches_count - 1 : 0;
  }
}

// Write helper for mpv IPC socket
static void send_mpv_cmd(int sock, const char *cmd) {
  if (sock < 0)
    return;
  size_t len = strlen(cmd);
  size_t total_sent = 0;
  while (total_sent < len) {
    ssize_t sent = write(sock, cmd + total_sent, len - total_sent);
    if (sent <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1000);
        continue;
      }
      break;
    }
    total_sent += sent;
  }
}

// Plays a song by index in original SongList
static void play_song(SongList *songs, size_t index) {
  if (index >= songs->len)
    return;

  state.active_song_index = (int)index;
  strncpy(state.current_song_path, songs->items[index],
          sizeof(state.current_song_path) - 1);

  const char *basename = get_basename(songs->items[index]);
  strncpy(state.current_song_title, basename,
          sizeof(state.current_song_title) - 1);

  state.time_pos = 0.0;
  state.duration = 0.0;

  // JSON escape the path
  char escaped_path[PATH_MAX * 2];
  char *dest = escaped_path;
  const char *src = state.current_song_path;
  while (*src != '\0') {
    if (*src == '"' || *src == '\\') {
      *dest++ = '\\';
    }
    *dest++ = *src++;
  }
  *dest = '\0';

  // Force immediate replacement and playback of the currently playing song
  char cmd[PATH_MAX * 2 + 128];
  snprintf(cmd, sizeof(cmd),
           "{\"command\": [\"loadfile\", \"%s\", \"replace\"]}\n",
           escaped_path);
  send_mpv_cmd(state.mpv_sock, cmd);

  // Explicitly unpause to guarantee immediate playback
  send_mpv_cmd(state.mpv_sock,
               "{\"command\": [\"set_property\", \"pause\", false]}\n");
}

// Parses JSON properties received from mpv over socket
static void parse_mpv_message(const char *line, SongList *songs) {
  if (strstr(line, "\"event\":\"end-file\"") != NULL) {
    // Continuous playback: play next song in matches list if possible,
    // otherwise fallback to original list CRITICAL: Only trigger continuous
    // playback if the song finished naturally ("reason":"eof"). This avoids
    // triggers caused by manual replacements ("reason":"stop" or
    // "reason":"redirect").
    if (strstr(line, "\"reason\":\"eof\"") != NULL) {
      if (state.active_song_index >= 0 && songs->len > 0) {
        size_t next_idx = (state.active_song_index + 1) % songs->len;

        if (state.matches_count > 0) {
          int found_idx = -1;
          for (size_t i = 0; i < state.matches_count; i++) {
            if ((int)state.matches[i].song_index == state.active_song_index) {
              found_idx = (int)i;
              break;
            }
          }
          if (found_idx >= 0) {
            next_idx =
                state.matches[(found_idx + 1) % state.matches_count].song_index;
          }
        }
        play_song(songs, next_idx);
      }
    }
    return;
  }

  if (strstr(line, "\"event\":\"property-change\"") != NULL) {
    char *name_ptr = strstr(line, "\"name\":\"");
    if (name_ptr) {
      name_ptr += 8;
      if (strncmp(name_ptr, "time-pos", 8) == 0) {
        char *data_ptr = strstr(line, "\"data\":");
        if (data_ptr) {
          sscanf(data_ptr + 7, "%lf", &state.time_pos);
        }
      } else if (strncmp(name_ptr, "duration", 8) == 0) {
        char *data_ptr = strstr(line, "\"data\":");
        if (data_ptr) {
          if (strncmp(data_ptr + 7, "null", 4) == 0) {
            state.duration = 0.0;
          } else {
            sscanf(data_ptr + 7, "%lf", &state.duration);
          }
        }
      } else if (strncmp(name_ptr, "pause", 5) == 0) {
        char *data_ptr = strstr(line, "\"data\":");
        if (data_ptr) {
          if (strncmp(data_ptr + 7, "true", 4) == 0) {
            state.is_paused = true;
          } else if (strncmp(data_ptr + 7, "false", 5) == 0) {
            state.is_paused = false;
          }
        }
      } else if (strncmp(name_ptr, "volume", 6) == 0) {
        char *data_ptr = strstr(line, "\"data\":");
        if (data_ptr) {
          sscanf(data_ptr + 7, "%lf", &state.volume);
        }
      }
    }
  }
}

// Render the interactive layout of our interface
static void draw_ui(SongList *songs) {
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  int cols = w.ws_col;
  int rows = w.ws_row;

  // Require a minimum size
  if (rows < 12 || cols < 40) {
    printf("\033[H\033[2J");
    printf("Terminal size too small!\r\n");
    printf("Current: %dx%d (Min: 40x12)\r\n", cols, rows);
    fflush(stdout);
    return;
  }

  // Clear screen smoothly by moving cursor to top-left and overwriting
  printf("\033[H");

  // 1. Sleek lavender centered header with Vim Mode Indicator
  char mode_indicator[64];
  snprintf(mode_indicator, sizeof(mode_indicator), " [VIM: %s] ",
           state.vim_mode ? "NORMAL" : "INSERT");

  char title[128] = " FUZZY AUDIO PLAYER ";
  int total_hdr_len = (int)strlen(title) + (int)strlen(mode_indicator) + 4;
  int padding = (cols - total_hdr_len) / 2;
  if (padding < 0)
    padding = 0;

  for (int i = 0; i < padding; i++)
    printf(" ");
  printf("\033[1;35m🎵 %s 🎵\033[0m", title);
  printf("\033[1;33m%s\033[0m\033[K\r\n", mode_indicator);

  // Dim gray separator
  printf("\033[90m");
  for (int i = 0; i < cols; i++)
    printf("─");
  printf("\033[0m\033[K\r\n");

  // 2. Cyan search prompt and user query
  printf("\033[1;36m🔍 Search: \033[0m");
  printf("\033[1;37m%s\033[0m", state.query);
  printf("\033[K\r\n");

  // Dim gray separator
  printf("\033[90m");
  for (int i = 0; i < cols; i++)
    printf("─");
  printf("\033[0m\033[K\r\n");

  // 3. Song Selection List
  int list_height = rows - 10;
  if (list_height < 3)
    list_height = 3;

  // Scroll handling
  if (state.selected_match_index < state.scroll_offset) {
    state.scroll_offset = state.selected_match_index;
  }
  if (state.selected_match_index >= state.scroll_offset + list_height) {
    state.scroll_offset = state.selected_match_index - list_height + 1;
  }

  for (int i = 0; i < list_height; i++) {
    size_t idx = state.scroll_offset + i;
    if (idx < state.matches_count) {
      MatchResult res = state.matches[idx];
      const char *full_path = songs->items[res.song_index];
      const char *song_name = get_basename(full_path);

      bool is_selected = (idx == state.selected_match_index);
      bool is_active = ((int)res.song_index == state.active_song_index);

      if (is_selected) {
        printf("\033[7m"); // Reverse video (stands out beautifully)
        if (is_active) {
          printf("\033[1;32m▶ \033[0m\033[7m"); // Active play mark
        } else {
          printf("  ");
        }
      } else {
        if (is_active) {
          printf("\033[1;32m▶ \033[0m");
        } else {
          printf("  ");
        }
      }

      // Subsequence character highlighting
      const char *q = state.query;
      const char *t = song_name;
      while (*t != '\0') {
        char t_char = *t;
        char q_char = *q;
        if (q_char >= 'A' && q_char <= 'Z')
          q_char += 32;
        char t_char_lower = t_char;
        if (t_char_lower >= 'A' && t_char_lower <= 'Z')
          t_char_lower += 32;

        if (*q != '\0' && t_char_lower == q_char) {
          if (is_selected) {
            printf("\033[1;36m%c\033[0m\033[7m",
                   t_char); // Bold Cyan Highlight inside selected
          } else {
            printf("\033[1;36m%c\033[0m", t_char); // Bold Cyan Highlight
          }
          q++;
        } else {
          printf("%c", t_char);
        }
        t++;
      }
      printf("\033[0m\033[K\r\n");
    } else {
      // Unused lines
      printf("\033[K\r\n");
    }
  }

  // Dim gray separator
  printf("\033[90m");
  for (int i = 0; i < cols; i++)
    printf("─");
  printf("\033[0m\033[K\r\n");

  // 4. Now Playing Info
  printf("\033[1;37mNow Playing: \033[0m");
  if (state.active_song_index >= 0) {
    printf("\033[1;35m%s\033[0m", state.current_song_title);
  } else {
    printf("\033[90mNone - Select a song and press Enter\033[0m");
  }
  printf("\033[K\r\n");

  // 5. Sleek visual seek progress bar
  int elapsed_min = (int)state.time_pos / 60;
  int elapsed_sec = (int)state.time_pos % 60;
  int duration_min = (int)state.duration / 60;
  int duration_sec = (int)state.duration % 60;

  double pct = 0.0;
  if (state.duration > 0.0) {
    pct = state.time_pos / state.duration;
    if (pct > 1.0)
      pct = 1.0;
    if (pct < 0.0)
      pct = 0.0;
  }

  int bar_width = cols - 18;
  if (bar_width < 10)
    bar_width = 10;
  int filled_width = (int)(pct * bar_width);

  printf("  %02d:%02d ", elapsed_min, elapsed_sec);
  printf("\033[1;35m"); // Lavender progress color
  for (int i = 0; i < bar_width; i++) {
    if (i < filled_width) {
      printf("━");
    } else if (i == filled_width) {
      printf("╸");
    } else {
      printf("\033[90m━\033[1;35m");
    }
  }
  printf("\033[0m");
  printf(" %02d:%02d\033[K\r\n", duration_min, duration_sec);

  // 6. Notification Banner OR Status visualizer and volume
  if (time(NULL) < state.notification_expiry) {
    printf("  \033[1;33m🔔 %s\033[0m\033[K\r\n", state.notification);
  } else {
    printf("  [");
    if (state.active_song_index < 0) {
      printf("\033[90mSTOPPED\033[0m");
    } else if (state.is_paused) {
      printf("\033[1;33mPAUSED \033[0m");
    } else {
      printf("\033[1;32mPLAYING\033[0m");
    }
    printf("]  ");

    // Animated simulated wave visualizer
    printf(" \033[1;36m");
    if (state.active_song_index >= 0 && !state.is_paused) {
      const char *bars[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
      for (int i = 0; i < 9; i++) {
        double angle = (state.frame_count * 0.4) + (i * 0.8);
        double val = (sin(angle) + 1.0) / 2.0;
        int bar_idx = (int)(val * 8);
        if (bar_idx < 0)
          bar_idx = 0;
        if (bar_idx > 8)
          bar_idx = 8;
        printf("%s", bars[bar_idx]);
      }
    } else {
      printf("▂▂▃▃▄▃▃▂▂");
    }
    printf("\033[0m  ");

    // Volume visualizer
    int vol_bar_filled = (int)((state.volume / 130.0) * 8);
    if (vol_bar_filled < 0)
      vol_bar_filled = 0;
    if (vol_bar_filled > 8)
      vol_bar_filled = 8;

    printf("🔊 %3d%% [", (int)state.volume);
    printf("\033[1;32m"); // Green volume bar
    for (int i = 0; i < 8; i++) {
      if (i < vol_bar_filled) {
        printf("█");
      } else {
        printf("\033[90m░\033[1;32m");
      }
    }
    printf("\033[0m]\033[K\r\n");
  }

  // Dim gray separator
  printf("\033[90m");
  for (int i = 0; i < cols; i++)
    printf("─");
  printf("\033[0m\033[K\r\n");

  // 7. Interactive Help Line
  if (state.vim_mode) {
    printf("\033[90m [j/k] Down/Up  [p] Play/Pause  [s] Shuffle  [r] Random  "
           "[i] Insert  [q] Quit\033[0m\033[K");
  } else {
    printf("\033[90m [Esc] Normal Mode  [Enter] Play  [Space] Pause  [[/]] Vol "
           " [q] Quit\033[0m\033[K");
  }

  fflush(stdout);
}

// Start player and run the interactive loop
void start_player(SongList *songs) {
  if (songs->len == 0) {
    printf("No audio files found in directory. Exiting...\n");
    return;
  }

  // Seed random number generator
  srand(time(NULL));

  // 1. Initialize State
  memset(&state, 0, sizeof(state));
  state.mpv_sock = -1;
  state.active_song_index = -1;
  state.volume = 60.0;
  state.running = true;
  state.vim_mode = false; // Start in search mode (Insert mode)

  state.matches = malloc(songs->len * sizeof(MatchResult));
  if (!state.matches) {
    perror("Failed to allocate matches buffer");
    return;
  }

  update_matches(songs);

  // Generate unique socket path in /tmp
  snprintf(state.mpv_socket_path, sizeof(state.mpv_socket_path),
           "/tmp/fzp-mpv-%d.sock", getpid());

  // 2. Spawn headless mpv background daemon (no video, no VO popup windows)
  state.mpv_pid = fork();
  if (state.mpv_pid == 0) {
    // Child: Daemonize mpv
    int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }

    char socket_arg[PATH_MAX + 32];
    snprintf(socket_arg, sizeof(socket_arg), "--input-ipc-server=%s",
             state.mpv_socket_path);

    char *args[] = {"mpv",         "--idle", socket_arg, "--no-terminal",
                    "--video=no", // Completely disable video tracks (removes
                                  // album art popups)
                    "--vo=null",  // Completely disable video output drivers
                                  // (removes album art windows)
                    "--volume=60", NULL};

    execvp("mpv", args);
    perror("execvp mpv failed");
    exit(EXIT_FAILURE);
  }

  if (state.mpv_pid < 0) {
    perror("fork mpv failed");
    free(state.matches);
    return;
  }

  // Register exit cleanup and system signal handlers
  atexit(cleanup_player);
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  signal(SIGQUIT, handle_signal);

  // 3. Connect to mpv IPC socket (try up to 20 times, waiting 50ms each)
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, state.mpv_socket_path, sizeof(addr.sun_path) - 1);

  int retries = 20;
  while (retries > 0) {
    state.mpv_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state.mpv_sock >= 0) {
      if (connect(state.mpv_sock, (struct sockaddr *)&addr, sizeof(addr)) ==
          0) {
        break; // Connected!
      }
      close(state.mpv_sock);
      state.mpv_sock = -1;
    }
    usleep(50000); // Wait 50ms
    retries--;
  }

  if (state.mpv_sock < 0) {
    fprintf(stderr,
            "Error: Could not connect to background mpv IPC socket.\r\n");
    exit(EXIT_FAILURE);
  }

  // Set socket to non-blocking
  int flags = fcntl(state.mpv_sock, F_GETFL, 0);
  fcntl(state.mpv_sock, F_SETFL, flags | O_NONBLOCK);

  // Register for property updates from mpv
  send_mpv_cmd(state.mpv_sock,
               "{\"command\": [\"observe_property\", 1, \"time-pos\"]}\n");
  send_mpv_cmd(state.mpv_sock,
               "{\"command\": [\"observe_property\", 2, \"duration\"]}\n");
  send_mpv_cmd(state.mpv_sock,
               "{\"command\": [\"observe_property\", 3, \"pause\"]}\n");
  send_mpv_cmd(state.mpv_sock,
               "{\"command\": [\"observe_property\", 4, \"volume\"]}\n");

  // Enable raw terminal mode
  enable_raw_mode();

  // Clear screen initially
  printf("\033[H\033[2J");
  fflush(stdout);

  // Buffered socket read variables
  char mpv_read_buf[4096] = {0};
  int mpv_read_len = 0;

  struct pollfd fds[2];
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[1].fd = state.mpv_sock;
  fds[1].events = POLLIN;

  draw_ui(songs);

  // Main poll loop
  while (state.running) {
    // Poll stdin and socket with 100ms timeout (10Hz frame rate for visualizer)
    int poll_ret = poll(fds, 2, 100);

    bool need_redraw = false;

    if (poll_ret < 0) {
      if (errno == EINTR)
        continue;
      break; // Socket error or signal
    }

    // Timeout ticked (100ms elapsed) - update animation frame
    if (poll_ret == 0) {
      state.frame_count++;
      need_redraw = true;
    }

    // Handle Socket IPC Updates from mpv
    if (fds[1].revents & POLLIN) {
      int space = sizeof(mpv_read_buf) - mpv_read_len - 1;
      if (space > 0) {
        ssize_t n = read(state.mpv_sock, mpv_read_buf + mpv_read_len, space);
        if (n > 0) {
          mpv_read_len += n;
          mpv_read_buf[mpv_read_len] = '\0';

          char *line_start = mpv_read_buf;
          char *newline;
          while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            parse_mpv_message(line_start, songs);
            line_start = newline + 1;
            need_redraw = true;
          }

          int consumed = line_start - mpv_read_buf;
          if (consumed > 0) {
            memmove(mpv_read_buf, line_start, mpv_read_len - consumed);
            mpv_read_len -= consumed;
            mpv_read_buf[mpv_read_len] = '\0';
          }
        } else if (n == 0 ||
                   (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          // Socket closed or error: break main loop
          state.running = false;
        }
      }
    }

    // Handle Keyboard input
    if (fds[0].revents & POLLIN) {
      char c;
      if (read(STDIN_FILENO, &c, 1) == 1) {
        need_redraw = true;

        // --- ESCAPE SEQUENCE HANDLER ---
        if (c == '\033') { // Escape character
          // Check if it's an arrow key sequence
          char seq[2];
          if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
              read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[0] == '[') {
              switch (seq[1]) {
              case 'A': // Arrow Up: select previous song
                if (state.selected_match_index > 0) {
                  state.selected_match_index--;
                }
                break;
              case 'B': // Arrow Down: select next song
                if (state.selected_match_index + 1 < state.matches_count) {
                  state.selected_match_index++;
                }
                break;
              case 'C': // Arrow Right: Seek forward 10s
                send_mpv_cmd(state.mpv_sock, "{\"command\": [\"seek\", 10]}\n");
                break;
              case 'D': // Arrow Left: Seek backward 10s
                send_mpv_cmd(state.mpv_sock,
                             "{\"command\": [\"seek\", -10]}\n");
                break;
              }
            }
          } else {
            // Just the single Escape key pressed
            if (!state.vim_mode) {
              // Search Mode -> Vim Command Mode
              state.vim_mode = true;
              show_notification("VIM NORMAL MODE");
            } else {
              // Vim Command Mode -> clear query
              state.query[0] = '\0';
              state.query_len = 0;
              update_matches(songs);
              show_notification("Query Cleared");
            }
          }
        }

        // --- VIM COMMAND MODE INPUTS ---
        else if (state.vim_mode) {
          if (c == 'q' || c == 3) { // 'q' or Ctrl-C: exit
            state.running = false;
          } else if (c == 'j') { // 'j': select next song
            if (state.selected_match_index + 1 < state.matches_count) {
              state.selected_match_index++;
            }
          } else if (c == 'k') { // 'k': select previous song
            if (state.selected_match_index > 0) {
              state.selected_match_index--;
            }
          } else if (c == 'p') { // 'p': toggle play/pause
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"cycle\", \"pause\"]}\n");
            show_notification(state.is_paused ? "Resumed Playback"
                                              : "Paused Playback");
          } else if (c == 's') { // 's': shuffle tracks
            shuffle_songs(songs);
            update_matches(songs);
            show_notification("Playlist Shuffled!");
          } else if (c == 'r') { // 'r': play random song
            play_random_song(songs);
          } else if (c == 'i' || c == 'a' ||
                     c == '/') { // switch to Search/Insert mode
            state.vim_mode = false;
            show_notification("VIM INSERT MODE");
          } else if (c == '\r' || c == '\n') { // Enter: play selected
            if (state.matches_count > 0 &&
                state.selected_match_index < state.matches_count) {
              size_t orig_idx =
                  state.matches[state.selected_match_index].song_index;
              play_song(songs, orig_idx);
            }
          } else if (c == '[') { // '[': Volume down 5%
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"add\", \"volume\", -5]}\n");
          } else if (c == ']') { // ']': Volume up 5%
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"add\", \"volume\", 5]}\n");
          }
        }

        // --- VIM SEARCH / INSERT MODE INPUTS ---
        else {
          if (c == 'q' || c == 3) { // 'q' or Ctrl-C: exit
            state.running = false;
          } else if (c == 127 || c == 8) { // Backspace
            if (state.query_len > 0) {
              state.query_len--;
              state.query[state.query_len] = '\0';
              update_matches(songs);
            }
          } else if (c == '\r' || c == '\n') { // Enter: play selected song
            if (state.matches_count > 0 &&
                state.selected_match_index < state.matches_count) {
              size_t orig_idx =
                  state.matches[state.selected_match_index].song_index;
              play_song(songs, orig_idx);
            }
          } else if (c == ' ') { // Space: pause / resume
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"cycle\", \"pause\"]}\n");
          } else if (c == '[') { // '[': Volume down 5%
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"add\", \"volume\", -5]}\n");
          } else if (c == ']') { // ']': Volume up 5%
            send_mpv_cmd(state.mpv_sock,
                         "{\"command\": [\"add\", \"volume\", 5]}\n");
          } else if (c >= 32 &&
                     c < 127) { // Typable alphanumeric search characters
            if (state.query_len < (int)sizeof(state.query) - 1) {
              state.query[state.query_len++] = c;
              state.query[state.query_len] = '\0';
              update_matches(songs);
            }
          }
        }
      }
    }

    if (need_redraw) {
      draw_ui(songs);
    }
  }
}
