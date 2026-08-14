/* stdio.h - Minimal stdio declarations: printf, snprintf, puts, FILE stub. */
#ifndef WASMOS_LIBC_STDIO_H
#define WASMOS_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stream handle. `fd` is the underlying FS descriptor, `mode` the read/write
 * rights fopen granted, and eof/error are sticky flags cleared only by
 * clearerr(). There is no user-space buffering: every stream call is an
 * immediate read()/write(). Streams live in a fixed pool owned by libc, so a
 * FILE* stays valid until fclose() and must never be freed by the caller. */
typedef struct {
    int fd;
    int mode;
    int eof;
    int error;
} FILE;

#define EOF (-1)

/* Write `len` bytes (embedded NULs included) to the console. Returns 0 on
 * success — a status, not a byte count — and a negative host-call code on
 * failure. A NULL `s`, a zero `len`, or a `len` above INT32_MAX is reported as
 * success without writing anything. */
int putsn(const char* s, size_t len);
/* Write `s` followed by a newline to the console. Returns 0 on success (not a
 * non-negative count), negative on the first failing write. */
int puts(const char* s);
/* Write `s` to `stream` with no trailing newline. Returns 0 on success, -1 on a
 * NULL/closed stream or a failed or short write, setting the stream's error
 * flag in the latter cases. */
int fputs(const char* s, FILE* stream);
/* Write one byte to stdout. Returns the byte as an unsigned char value, or EOF
 * if the write did not report exactly one byte. */
int putchar(int ch);
/* Read one byte from stdin. Returns the byte, or EOF both on error and when no
 * input is currently buffered: the console read does not block, so EOF here
 * means "nothing available right now", not necessarily end of input. */
int getchar(void);
/* Read bytes from stdin into `s` until a newline (kept in the buffer), `size`-1
 * bytes, or the console runs dry; the result is always NUL-terminated. Returns
 * the number of bytes stored, or -1 for a NULL `s`, a `size` of 1 or less, or a
 * read error. Because the console read does not block, a partial line can be
 * returned and the caller must keep calling to assemble a full one. */
int readline(char* s, int size);
/* Format into `buffer`, writing at most `size`-1 characters plus a terminating
 * NUL. Returns the length the formatted output would have had, excluding the
 * NUL (so a value >= size means truncation), or -1 on a NULL format or a format
 * string ending in a bare '%', in which case buffer[0] is set to '\0'.
 * A NULL `buffer` or a `size` of 0 is allowed and only computes the length.
 * Supported: %c %s %d %i %u %x %X %p %%, an optional '0' pad flag, a decimal
 * minimum field width, and the l/ll/z length modifiers. No precision, no
 * left-justification, no floating point; an unknown conversion is echoed
 * verbatim. A NULL %s argument prints "(null)". The ll modifier fetches a long
 * long but narrows it to long before formatting, so on wasm32, where long is 32
 * bits, a %lld or %llx value wider than 32 bits prints truncated. */
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
int snprintf(char* buffer, size_t size, const char* format, ...);
/* Format to the console through 256-byte batched writes, same conversions as
 * vsnprintf. Returns the number of characters emitted, -1 on a malformed format
 * string, or the negative host-call code of the first failed console write. */
int vprintf(const char* format, va_list args);
int printf(const char* format, ...);
/* Open `path` as a stream. `mode` must be one of "r"/"rb" (read), "w"/"wb"
 * (create/truncate for writing) or "a"/"ab" (create/append); update modes are
 * rejected. Returns NULL on a bad mode, a failed open, or when all eight stream
 * slots are in use. */
FILE* fopen(const char* path, const char* mode);
/* Read/write `nmemb` items of `size` bytes. Returns the number of whole items
 * transferred, which is 0 on any error (with the stream's error flag set), on a
 * stream lacking the required mode, or for a NULL/zero argument. fread marks EOF
 * when it gets fewer bytes than requested. A trailing partial item is not
 * reported and its bytes are lost, since the byte count is divided by `size`. */
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
/* Close the stream and return its slot to the pool. Returns close()'s result,
 * or -1 for NULL or a pointer that is not a currently open stream. */
int fclose(FILE* stream);
/* Read up to `size`-1 bytes into `s`, stopping after a newline (which is kept).
 * Returns `s`, or NULL on a bad argument or when no byte could be read at all
 * (end of file or error — check feof/ferror to tell them apart). */
char* fgets(char* s, int size, FILE* stream);
/* Read one byte. Returns it as an unsigned char value, or EOF at end of file
 * (sets the eof flag) or on error (sets the error flag). */
int fgetc(FILE* stream);
int getc(FILE* stream);
/* Seek the stream. `whence` is SEEK_SET/SEEK_CUR/SEEK_END. Returns 0 on
 * success and clears both eof and error; on failure returns -1 and sets the
 * error flag. */
int fseek(FILE* stream, long offset, int whence);
/* Current offset, or -1 on a NULL/closed stream or a failed query (which sets
 * the error flag). */
long ftell(FILE* stream);
/* Report and clear the sticky flags. All three treat a NULL stream as a no-op,
 * reporting 0. */
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);

#ifdef __cplusplus
}
#endif

#endif
