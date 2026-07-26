/* stdio.h — emlibc. Buffered streams over the EmbLink read/write/open rim.
 * The printf family is agnostic formatting; only the FILE→fd retargeting
 * underneath is EmbLink-specific. FILE is opaque; stdin/stdout/stderr are
 * real streams on fds 0/1/2. */
#ifndef _EMLIBC_STDIO_H
#define _EMLIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _EM_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* output */
int     fputc(int c, FILE *f);
int     putc(int c, FILE *f);
int     putchar(int c);
int     fputs(const char *s, FILE *f);
int     puts(const char *s);
size_t  fwrite(const void *p, size_t size, size_t nmemb, FILE *f);
int     fflush(FILE *f);

/* input */
int     fgetc(FILE *f);
int     getchar(void);
char   *fgets(char *buf, int n, FILE *f);
size_t  fread(void *p, size_t size, size_t nmemb, FILE *f);

/* formatting */
int     printf(const char *fmt, ...);
int     fprintf(FILE *f, const char *fmt, ...);
int     vprintf(const char *fmt, va_list ap);
int     vfprintf(FILE *f, const char *fmt, va_list ap);
int     snprintf(char *buf, size_t cap, const char *fmt, ...);
int     vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int     sprintf(char *buf, const char *fmt, ...);

/* streams */
FILE   *fopen(const char *path, const char *mode);
int     fclose(FILE *f);
int     feof(FILE *f);
int     ferror(FILE *f);

#endif /* _EMLIBC_STDIO_H */
