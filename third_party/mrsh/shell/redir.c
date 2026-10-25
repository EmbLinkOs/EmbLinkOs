#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include "shell/redir.h"
#include "native.h"

static ssize_t write_here_document_line(int fd, struct mrsh_word *line,
		ssize_t max_size) {
	char *line_str = mrsh_word_str(line);
	size_t line_len = strlen(line_str);
	size_t write_len = line_len + 1; // line + terminating \n
	if (max_size >= 0 && write_len > (size_t)max_size) {
		free(line_str);
		return 0;
	}

	errno = 0;
	ssize_t n = write(fd, line_str, line_len);
	free(line_str);
	if (n < 0 || (size_t)n != line_len) {
		goto err_write;
	}

	if (write(fd, "\n", sizeof(char)) != 1) {
		goto err_write;
	}

	return write_len;

err_write:
	fprintf(stderr, "write() failed: %s\n",
		errno ? strerror(errno) : "short write");
	return -1;
}

static int create_here_document_fd(const struct mrsh_array *lines) {
    int fd=sh_anonymous_file();if(fd<0)return -1;
    for(size_t i=0;i<lines->len;i++)if(write_here_document_line(fd,lines->data[i],-1)<0){close(fd);return -1;}
    if(lseek(fd,0,SEEK_SET)<0){close(fd);return -1;}return fd;
}

static int parse_fd(const char *str) {
	char *endptr;
	errno = 0;
	int fd = strtol(str, &endptr, 10);
	if (errno != 0) {
		return -1;
	}
	if (endptr[0] != '\0') {
		errno = EINVAL;
		return -1;
	}

	return fd;
}

int process_redir(const struct mrsh_io_redirect *redir, int *redir_fd) {
	// TODO: filename expansions
	char *filename = mrsh_word_str(redir->name);

	int fd = -1, default_redir_fd = -1;
	errno = 0;
	switch (redir->op) {
	case MRSH_IO_LESS: // <
		fd = open(filename, O_CLOEXEC | O_RDONLY);
		default_redir_fd = STDIN_FILENO;
		break;
	case MRSH_IO_GREAT: // >
	case MRSH_IO_CLOBBER: // >|
		fd = open(filename,
			O_CLOEXEC | O_WRONLY | O_CREAT | O_TRUNC, 0644);
		default_redir_fd = STDOUT_FILENO;
		break;
	case MRSH_IO_DGREAT: // >>
		fd = open(filename,
			O_CLOEXEC | O_WRONLY | O_CREAT | O_APPEND, 0644);
		default_redir_fd = STDOUT_FILENO;
		break;
	case MRSH_IO_LESSAND: // <&
		// TODO: parse "-"
		fd = parse_fd(filename);
		default_redir_fd = STDIN_FILENO;
		break;
	case MRSH_IO_GREATAND: // >&
		// TODO: parse "-"
		fd = parse_fd(filename);
		default_redir_fd = STDOUT_FILENO;
		break;
	case MRSH_IO_LESSGREAT: // <>
		fd = open(filename, O_CLOEXEC | O_RDWR | O_CREAT, 0644);
		default_redir_fd = STDIN_FILENO;
		break;
	case MRSH_IO_DLESS: // <<
	case MRSH_IO_DLESSDASH: // <<-
		fd = create_here_document_fd(&redir->here_document);
		default_redir_fd = STDIN_FILENO;
		break;
	}
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", filename,
			strerror(errno));
        free(filename);
		return -1;
	}

	free(filename);

	*redir_fd = redir->io_number;
	if (*redir_fd < 0) {
		*redir_fd = default_redir_fd;
	}

	return fd;
}
