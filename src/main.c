#include "terminal.h"
#include <pthread.h>

#define BOARD 20
typedef struct {
  pthread_mutex_t lock;
  data_2d cells[BOARD * BOARD];
  int size;
  int running;
  char inputs[10];
} thread_args;
typedef struct {
	data_2d minos[4];
	Vec2d g_position;
} pieces;

pieces genPiece(){
	pieces piece;
	return piece;
};

void genBoard(thread_args *args) {
  int n = 0;
  for (int y = 0; y < BOARD; y++)
    for (int x = 0; x < BOARD; x++)
      if (x == 0 || x == BOARD - 1 || y == 0 || y == BOARD - 1)
        args->cells[n++] =
            (data_2d){{(double)x, (double)y}, {255.0, 255.0, 255.0}, '#'};
  args->size = n;
}
void *drawThread(void *args) {
  thread_args *arg = (thread_args *)args;
  int length = arg->size;
  Arena arena = arena_create(1 << 16);

  while (arg->running == 1) {
    arena_reset(&arena);
    if (pthread_mutex_trylock(&arg->lock) == 0) {
      order(arg->cells, length);
      char_run *runs = organize(&arena, arg->cells, length);
      printf(TERM_CLEAR TERM_HOME);
      printScreen(runs, length);
      printf("\x1b[%d;1H\n", BOARD + 1); // park the cursor below the board
      pthread_mutex_unlock(&arg->lock);
    }
    usleep(16000); // ~60 fps
  }
  arena_destroy(&arena);
  return NULL;
}
void *userInputs(void *args) {
  thread_args *arg = (thread_args *)args;
  char buf[sizeof(arg->inputs)];
  while (arg->running == 1) {
    memset(buf, 0, sizeof buf);
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n > 0 && pthread_mutex_trylock(&arg->lock) == 0) {
      memcpy(arg->inputs, buf, sizeof buf);
      pthread_mutex_unlock(&arg->lock);
    }
  }
  return NULL;
}
void *gameLoop(void *args) {
  thread_args *arg = (thread_args *)args;
  while (arg->running == 1) {
    if (pthread_mutex_trylock(&arg->lock) == 0) {
      for (int i = 0; i < 10; i++) {
        if (arg->inputs[i] == 'q') {
          arg->running = 0;
        }
      }
      pthread_mutex_unlock(&arg->lock);
    }
    usleep(16000);
  }
  return NULL;
}

int main(void) {
  pthread_t threads[3];
  thread_args arg;
  arg.running = 1;
  memset(arg.inputs, 0, sizeof arg.inputs);
  pthread_mutex_init(&arg.lock, NULL);
  genBoard(&arg);

  term_init();
  pthread_create(&threads[0], NULL, drawThread, &arg);
  pthread_create(&threads[1], NULL, userInputs, &arg);
  pthread_create(&threads[2], NULL, gameLoop, &arg);
  pthread_join(threads[0], NULL);
  pthread_join(threads[1], NULL);
  pthread_join(threads[2], NULL);
  term_restore();

  pthread_mutex_destroy(&arg.lock);
  return 0;
}
