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

int main(){
	return 0;
}
