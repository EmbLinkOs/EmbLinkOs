/* Interactive line interface.
 *
 * EmbLink: on a terminal the line is edited by the EmbLink shell's own editor
 * (shell/edit) -- history in ~/.local/share/shell/sh_history, completion
 * (ports/mrsh/sh_complete.c), multi-line programs decided by mrsh's own
 * parser -- and framed in the same OSC 133 marks the native shell writes, so
 * the Terminal is a screen for sh exactly as for the native shell. From a pipe
 * the lines are read as they come, strictly POSIX. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mrsh/shell.h>
#include <mrsh/parser.h>
#include "frontend.h"
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "edit/edit.h"
#include "hist/hist.h"
#include "sh_complete.h"
#ifdef EMBLINK_TARGET
#include "embk.h"
#define getline __getline
#endif

static struct editor *editor;
static bool first_prompt = true;
static char ahead[512];
static size_t ahead_n;

static void to_terminal(void *ctx, const char *bytes, size_t n) {
	(void)ctx;
	while (n) {
		ssize_t w = write(STDERR_FILENO, bytes, n);
		if (w <= 0) return;
		bytes += w;
		n -= (size_t)w;
	}
}

/* Unfinished: mrsh's parser asks for a continuation line. */
static bool unfinished(void *ctx, const char *text) {
	(void)ctx;
	size_t n = strlen(text);
	char *copy = malloc(n + 2);
	if (!copy) return false;
	memcpy(copy, text, n);
	copy[n] = '\n';
	copy[n + 1] = '\0';
	struct mrsh_parser *parser = mrsh_parser_with_data(copy, n + 1);
	bool more = false;
	while (parser) {
		struct mrsh_program *prog = mrsh_parse_line(parser);
		if (prog) {
			mrsh_program_destroy(prog);
			if (mrsh_parser_eof(parser)) break;
			continue;
		}
		more = mrsh_parser_continuation_line(parser);
		break;
	}
	if (parser) mrsh_parser_destroy(parser);
	free(copy);
	return more;
}

static int terminal_cols(void) {
	struct winsize ws;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
	return 80;
}

void interactive_init(struct mrsh_state *state) {
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) != 0) return;     /* a pipe: plain lines */
	struct edit_host host = { to_terminal, sh_complete, unfinished, state };
	editor = edit_new(&host);
	const char *home = getenv("HOME");
	if (home && home[0] && strcmp(home, "/")) {
		char path[256];
		static const char *const dirs[] = { "/.local", "/.local/share", "/.local/share/shell" };
		for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
			snprintf(path, sizeof path, "%s%s", home, dirs[i]);
			(void)mkdir(path, 0700);
		}
		snprintf(path, sizeof path, "%s/.local/share/shell/sh_history", home);
		(void)hist_open(path);
	}
	/* A prompt that says where you are, as the native shell's does, unless
	 * PS1 is the user's own. */
	if (!mrsh_env_get(state, "PS1", NULL))
		mrsh_env_set(state, "PS1", "\033[1;33m${USER:-user}@${HOSTNAME:-emblink}:${PWD}\033[0m sh$ ",
			MRSH_VAR_ATTRIB_NONE);
}

static bool wait_input(int ms) {
	for (int waited = 0; waited < ms; waited += 5) {
#ifdef EMBLINK_TARGET
		if (embk_fd_poll(STDIN_FILENO, EMBK_POLLIN) & EMBK_POLLIN) return true;
		embk_sleep_ms(5);
#else
		usleep(5000);
#endif
	}
	return false;
}

static size_t plain_next(char **line, const char *prompt) {
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	size_t len = 0;
	char *_line = NULL;
	errno = 0;
	ssize_t n_read = getline(&_line, &len, stdin);
	if (n_read < 0) {
		free(_line);
		if (errno != 0) {
			perror("getline");
		}
		return 0;
	}
	*line = _line;
	return n_read;
}

size_t interactive_next(struct mrsh_state *state,
		char **line, const char *prompt) {
	*line = NULL;
	if (!editor) return plain_next(line, prompt);

	char out[160];
	if (!first_prompt) {
		snprintf(out, sizeof out, "\033]133;D;%d\007", state->last_status);
		to_terminal(NULL, out, strlen(out));
	}
	first_prompt = false;
	/* PROMPT_SP, as the native shell does it: a prompt never starts at the
	 * end of output that did not finish its line. */
	int cols = terminal_cols();
	char fill[2112];
	int k = snprintf(fill, sizeof fill, "\033[?1049l\033[?1l\033>\033[?25h\033[0m\033[5 q\033[1;7m%%\033[0m");
	for (int i = 1; i < cols && k < (int)sizeof fill - 8; i++) fill[k++] = ' ';
	memcpy(fill + k, "\r \r", 3);
	to_terminal(NULL, fill, (size_t)k + 3);

	char cwd[256], mark[300];
	if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
	for (char *p = cwd; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = '?';
	snprintf(mark, sizeof mark, "\033]133;A;edit=shell;cwd=%s\007", cwd);
	const char *program = getenv("TERM_PROGRAM");
	bool ours = program && !strcmp(program, "EmbLinkTerminal");
	struct edit_prompt p = { mark, prompt, "\033[2m...\033[0m ", "\033]133;B\007", ours ? "\033]133;R\007" : NULL };

	/* Every key is the editor's while the line is edited; whatever the
	 * terminal was set to (a program before may have changed it) comes back
	 * before the command runs. */
	struct termios saved, editing;
	bool tty = tcgetattr(STDIN_FILENO, &saved) == 0;
	if (tty) {
		editing = saved;
		editing.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
		editing.c_iflag &= ~(tcflag_t)ICRNL;
		editing.c_oflag |= OPOST | ONLCR;
		editing.c_cc[VMIN] = 1;
		editing.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &editing);
	}
	edit_begin(editor, &p, EDIT_VT, cols);
	enum edit_result r = EDIT_MORE;
	if (ahead_n) {
		size_t used = 0;
		r = edit_feed(editor, ahead, ahead_n, &used);
		memmove(ahead, ahead + used, ahead_n - used);
		ahead_n -= used;
	}
	while (r == EDIT_MORE) {
		char buf[256];
		ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
		if (n <= 0) { r = EDIT_EOF; break; }
		size_t used = 0;
		r = edit_feed(editor, buf, (size_t)n, &used);
		if (r != EDIT_MORE) {
			size_t rest = (size_t)n - used;
			if (rest > sizeof ahead - ahead_n) rest = sizeof ahead - ahead_n;
			memcpy(ahead + ahead_n, buf + used, rest);
			ahead_n += rest;
		}
		while (r == EDIT_MORE && edit_pending_escape(editor) && !wait_input(40))
			r = edit_escape_timeout(editor);
	}
	if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	to_terminal(NULL, "\033[0 q", 5);
	if (r == EDIT_EOF) return 0;
	const char *text = r == EDIT_LINE ? edit_line(editor) : "";
	if (r == EDIT_CANCEL) state->last_status = 130;
	size_t n = strlen(text);
	char *result = malloc(n + 2);
	if (!result) return 0;
	memcpy(result, text, n);
	result[n] = '\n';
	result[n + 1] = '\0';
	if (r == EDIT_LINE) {
		hist_push(text);
		to_terminal(NULL, "\033]133;C\007", 9);
	}
	*line = result;
	return n + 1;
}
