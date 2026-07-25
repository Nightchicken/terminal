#ifndef TERMINAL_H
#define TERMINAL_H
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

// Raw-mode + alternate-screen control for the renderer's output thread.
// Header-only. term_init() saves the current tty state, enters the alternate
// screen, hides the cursor and switches the tty to raw mode; term_restore()
// undoes all of that and is safe to call more than once (atexit / signal).

// ANSI control sequences.
#define TERM_ALT_ON   "\x1b[?1049h"
#define TERM_ALT_OFF  "\x1b[?1049l"
#define TERM_HIDE_CUR "\x1b[?25l"
#define TERM_SHOW_CUR "\x1b[?25h"
#define TERM_CLEAR    "\x1b[2J"
#define TERM_HOME     "\x1b[H"

static struct termios term_saved;
static int term_saved_valid = 0;

// write() the whole string, tolerating short writes.
static void term_write(const char* s){
	size_t len = strlen(s);
	size_t off = 0;
	while (off < len){
		ssize_t n = write(STDOUT_FILENO, s + off, len - off);
		if (n <= 0) break;
		off += (size_t)n;
	}
}

// Current terminal size in character cells. Returns 0 on success, -1 on error.
static int term_size(int* cols, int* rows){
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return -1;
	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
}

static void term_init(void){
	if (tcgetattr(STDIN_FILENO, &term_saved) == 0) term_saved_valid = 1;

	struct termios raw = term_saved;
	// Raw mode: no canonical line editing, no echo, no signal-generating keys,
	// no CR/LF or output post-processing. VMIN=0/VTIME=1 => read() returns after
	// at most 100ms even with no input, so the input thread can poll `running`.
	raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(unsigned)(OPOST);
	raw.c_cflag |=  (unsigned)(CS8);
	raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN]  = 0;
	raw.c_cc[VTIME] = 1;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

	term_write(TERM_ALT_ON);
	term_write(TERM_HIDE_CUR);
	term_write(TERM_CLEAR);
}

static void term_restore(void){
	term_write(TERM_SHOW_CUR);
	term_write(TERM_ALT_OFF);
	if (term_saved_valid){
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
		term_saved_valid = 0;
	}
}

#endif

/*
 * MIT License
 * Copyright (c) 2026 Matthew Kabat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
