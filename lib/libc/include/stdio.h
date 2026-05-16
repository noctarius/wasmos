#ifndef WASMOS_LIBC_STDIO_H
#define WASMOS_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct {
    int fd;
    int mode;
    int eof;
    int error;
} FILE;

#define EOF (-1)

int putsn(const char *s, size_t len);
int puts(const char *s);
int fputs(const char *s, FILE *stream);
int putchar(int ch);
int getchar(void);
int readline(char *s, int size);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vprintf(const char *format, va_list args);
int printf(const char *format, ...);
FILE *fopen(const char *path, const char *mode);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fclose(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);

#endif
