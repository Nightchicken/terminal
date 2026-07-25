#ifndef TERMINAL_H
#define TERMINAL_H
#include "arena.h"
#include "vec.h"
#include <bits/pthreadtypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// ANSI control sequences.
#define TERM_ALT_ON "\x1b[?1049h"
#define TERM_ALT_OFF "\x1b[?1049l"
#define TERM_HIDE_CUR "\x1b[?25l"
#define TERM_SHOW_CUR "\x1b[?25h"
#define TERM_CLEAR "\x1b[2J"
#define TERM_HOME "\x1b[H"

static struct termios term_saved;
static int term_saved_valid = 0;

// Current terminal size in character cells. Returns 0 on success, -1 on error.
static inline int term_size(int *cols, int *rows) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

static inline void term_write(const char *s) {
  size_t len = strlen(s);
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(STDOUT_FILENO, s + off, len - off);
    if (n <= 0)
      break;
    off += (size_t)n;
  }
}

static inline void term_init(void) {
  if (tcgetattr(STDIN_FILENO, &term_saved) == 0)
    term_saved_valid = 1;

  struct termios raw = term_saved;
  // Raw mode: no canonical line editing, no echo, no signal-generating keys,
  // no CR/LF or output post-processing. VMIN=0/VTIME=1 => read() returns after
  // at most 100ms even with no input, so the input thread can poll `running`.
  raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_cflag |= (unsigned)(CS8);
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  term_write(TERM_ALT_ON);
  term_write(TERM_HIDE_CUR);
  term_write(TERM_CLEAR);
}

static inline void term_restore(void) {
  term_write(TERM_SHOW_CUR);
  term_write(TERM_ALT_OFF);
  if (term_saved_valid) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
    term_saved_valid = 0;
  }
}

// Render
typedef struct data_2d {
  Vec2d positon;
  Vec3d color;
  char c;
} data_2d;

typedef struct data_3d {
  Vec3d positon;
  Vec3d color;
  char c;
} data_3d;
typedef struct char_run {
  data_2d data;
  int run;
} char_run;

struct userInput{
	pthread_mutex_t lock;
	char inputs[10];
};

static inline int data_2d_before(data_2d a, data_2d b) {
  if (a.positon.y < b.positon.y)
    return 1;
  if (a.positon.y > b.positon.y)
    return 0;
  return a.positon.x < b.positon.x;
}

// In-place quicksort (Hoare partition), recursing into the smaller side and
// looping on the larger to bound stack depth at O(log n).
static inline void order_qsort(data_2d *data, int lo, int hi) {
  while (lo < hi) {
    data_2d pivot = data[lo + (hi - lo) / 2];
    int i = lo, j = hi;
    while (i <= j) {
      while (data_2d_before(data[i], pivot))
        i++;
      while (data_2d_before(pivot, data[j]))
        j--;
      if (i <= j) {
        data_2d tmp = data[i];
        data[i] = data[j];
        data[j] = tmp;
        i++;
        j--;
      }
    }
    if (j - lo < hi - i) {
      order_qsort(data, lo, j);
      lo = i;
    } else {
      order_qsort(data, i, hi);
      hi = j;
    }
  }
}

static inline void order(data_2d *data, int length){
  if (length < 2)
    return;
  order_qsort(data, 0, length - 1);
}

// Copy each cell into an arena-backed char_run array and group them into runs.
// A run is a maximal span of cells in the same row (positon.y) with consecutive
// columns (positon.x). Within a run, `run` counts down from length-1 to 0, so a
// cell's `run` is how many cells still follow it in that run. A row change or a
// gap in column number ends the run. Expects `data` pre-sorted by order().
static inline char_run *organize(Arena *arena, data_2d *data, int length) {
  char_run *run =
      (char_run *)arena_alloc(arena, (size_t)length * sizeof(char_run));
  int i = 0;
  while (i < length) {
    int j = i;
    while (j + 1 < length &&
           data[j + 1].positon.y == data[j].positon.y &&
           data[j + 1].positon.x == data[j].positon.x + 1)
      j++;
    for (int k = i; k <= j; k++) {
      run[k].data = data[k];
      run[k].run = j - k;
    }
    i = j + 1;
  }
  return run;
}

static inline void printScreen(char_run *data, int length) {
  char_run *run_data = data;
  for (int i = 0; i < length; i++) {
    printf("\x1b[%d;%dH", (int)run_data->data.positon.y + 1,
           (int)run_data->data.positon.x + 1);
    while (1) {
      Vec3d color = run_data->data.color;
      printf("\x1b[38;2;%d;%d;%dm%c", (int)color.x, (int)color.y, (int)color.z,
             run_data->data.c);
      int end = run_data->run == 0;
      run_data++;
      if (end)
        break;
      i++;
    }
  }
  printf("\x1b[0m");
  fflush(stdout);
}

#endif
