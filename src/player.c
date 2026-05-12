#include <termios.h>
#include <stdbool.h>
#include <unistd.h>

void set_conio_terminal_mode(bool enable) {
  static struct termios old_t;
  static bool is_raw = false;

  if (enable && !is_raw) {
    tcgetattr(STDIN_FILENO, &old_t);
    struct termios new_t = old_t;

    new_t.c_cflag &= ~(ICANON | ECHO);
    new_t.c_cc[VMIN] = 1;
    new_t.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
    is_raw = true;
  } else if (!enable && is_raw) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
    is_raw = false;
  }
}
