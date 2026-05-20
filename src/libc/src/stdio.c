#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"

#include <stdint.h>

typedef void (*stdio_emit_fn)(void *ctx, char ch);

typedef struct {
    char *buffer;
    size_t size;
    size_t pos;
    size_t total;
} stdio_buffer_t;

typedef struct {
    char data[256];
    size_t len;
    size_t total;
    int error;
} stdio_console_t;

static void
buffer_emit(void *ctx, char ch)
{
    stdio_buffer_t *buffer = (stdio_buffer_t *)ctx;

    if (!buffer) {
        return;
    }
    if (buffer->buffer && buffer->size > 0 && buffer->pos + 1 < buffer->size) {
        buffer->buffer[buffer->pos] = ch;
    }
    buffer->pos++;
    buffer->total++;
}

static void
console_flush(stdio_console_t *console)
{
    if (!console || console->error < 0 || console->len == 0) {
        return;
    }
    console->error = putsn(console->data, console->len);
    console->len = 0;
}

static void
console_emit(void *ctx, char ch)
{
    stdio_console_t *console = (stdio_console_t *)ctx;

    if (!console || console->error < 0) {
        return;
    }
    if (console->len >= sizeof(console->data)) {
        console_flush(console);
        if (console->error < 0) {
            return;
        }
    }
    console->data[console->len++] = ch;
    console->total++;
}

static void
emit_repeat(stdio_emit_fn emit, void *ctx, char ch, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        emit(ctx, ch);
    }
}

static void
emit_string(stdio_emit_fn emit, void *ctx, const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        emit(ctx, s[i]);
    }
}

static size_t
utoa_base(unsigned long value, unsigned int base, int uppercase, char *buffer, size_t size)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    size_t len = 0;

    if (!buffer || size == 0 || base < 2 || base > 16) {
        return 0;
    }

    do {
        if (len >= size) {
            return 0;
        }
        buffer[len++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (size_t i = 0; i < len / 2; ++i) {
        char tmp = buffer[i];
        buffer[i] = buffer[len - 1 - i];
        buffer[len - 1 - i] = tmp;
    }

    return len;
}

static void
format_unsigned(stdio_emit_fn emit,
                void *ctx,
                unsigned long value,
                unsigned int base,
                int uppercase,
                size_t width,
                char pad_char)
{
    char digits[32];
    size_t len = utoa_base(value, base, uppercase, digits, sizeof(digits));

    if (len == 0) {
        return;
    }
    if (width > len) {
        emit_repeat(emit, ctx, pad_char, width - len);
    }
    emit_string(emit, ctx, digits, len);
}

static void
format_signed(stdio_emit_fn emit, void *ctx, long value, size_t width, char pad_char)
{
    unsigned long magnitude = (unsigned long)value;
    char digits[32];
    size_t len;
    size_t pad = 0;
    int negative = 0;

    if (value < 0) {
        negative = 1;
        magnitude = (unsigned long)(-(value + 1)) + 1ul;
    }

    len = utoa_base(magnitude, 10u, 0, digits, sizeof(digits));
    if (len == 0) {
        return;
    }

    if (width > len + (size_t)negative) {
        pad = width - len - (size_t)negative;
    }

    if (negative && pad_char == '0') {
        emit(ctx, '-');
        emit_repeat(emit, ctx, pad_char, pad);
    } else {
        emit_repeat(emit, ctx, pad_char, pad);
        if (negative) {
            emit(ctx, '-');
        }
    }
    emit_string(emit, ctx, digits, len);
}

static int
vformat(stdio_emit_fn emit, void *ctx, const char *format, va_list args)
{
    va_list ap;

    if (!emit || !format) {
        return -1;
    }

    va_copy(ap, args);
    while (*format) {
        size_t width = 0;
        char pad_char = ' ';
        int long_flag = 0;

        if (*format != '%') {
            emit(ctx, *format++);
            continue;
        }

        format++;
        if (*format == '%') {
            emit(ctx, *format++);
            continue;
        }
        if (*format == '0') {
            pad_char = '0';
            format++;
        }
        while (*format >= '0' && *format <= '9') {
            width = (width * 10u) + (size_t)(*format - '0');
            format++;
        }
        if (*format == 'l') {
            long_flag = 1;
            format++;
        }

        switch (*format) {
            case 'c': {
                char ch = (char)va_arg(ap, int);
                if (width > 1) {
                    emit_repeat(emit, ctx, pad_char, width - 1);
                }
                emit(ctx, ch);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                size_t len = strlen(s ? s : "(null)");
                const char *out = s ? s : "(null)";
                if (width > len) {
                    emit_repeat(emit, ctx, pad_char, width - len);
                }
                emit_string(emit, ctx, out, len);
                break;
            }
            case 'd':
            case 'i':
                if (long_flag) {
                    format_signed(emit, ctx, va_arg(ap, long), width, pad_char);
                } else {
                    format_signed(emit, ctx, (long)va_arg(ap, int), width, pad_char);
                }
                break;
            case 'u':
                if (long_flag) {
                    format_unsigned(emit, ctx, va_arg(ap, unsigned long), 10u, 0, width, pad_char);
                } else {
                    format_unsigned(emit, ctx, (unsigned long)va_arg(ap, unsigned int), 10u, 0, width, pad_char);
                }
                break;
            case 'x':
            case 'X':
                if (long_flag) {
                    format_unsigned(emit,
                                    ctx,
                                    va_arg(ap, unsigned long),
                                    16u,
                                    *format == 'X',
                                    width,
                                    pad_char);
                } else {
                    format_unsigned(emit,
                                    ctx,
                                    (unsigned long)va_arg(ap, unsigned int),
                                    16u,
                                    *format == 'X',
                                    width,
                                    pad_char);
                }
                break;
            case 'p': {
                uintptr_t value = (uintptr_t)va_arg(ap, void *);
                emit_string(emit, ctx, "0x", 2);
                format_unsigned(emit, ctx, (unsigned long)value, 16u, 0, width, '0');
                break;
            }
            case '\0':
                va_end(ap);
                return -1;
            default:
                emit(ctx, '%');
                emit(ctx, *format);
                break;
        }

        if (*format) {
            format++;
        }
    }
    va_end(ap);
    return 0;
}

int
putsn(const char *s, size_t len)
{
    if (!s || len == 0 || len > 0x7FFFFFFFul) {
        return 0;
    }
    return wasmos_console_write((int32_t)(uintptr_t)s, (int32_t)len);
}

int
puts(const char *s)
{
    static const char newline = '\n';
    int rc = putsn(s, strlen(s));

    if (rc < 0) {
        return rc;
    }
    return putsn(&newline, 1);
}

int
vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    stdio_buffer_t out = { buffer, size, 0, 0 };

    if (vformat(buffer_emit, &out, format, args) < 0) {
        if (buffer && size > 0) {
            buffer[0] = '\0';
        }
        return -1;
    }

    if (buffer && size > 0) {
        size_t end = (out.pos < size) ? out.pos : (size - 1);
        buffer[end] = '\0';
    }
    return (int)out.total;
}

int
snprintf(char *buffer, size_t size, const char *format, ...)
{
    int rc;
    va_list args;

    va_start(args, format);
    rc = vsnprintf(buffer, size, format, args);
    va_end(args);
    return rc;
}

int
vprintf(const char *format, va_list args)
{
    stdio_console_t out = { {0}, 0, 0, 0 };

    if (vformat(console_emit, &out, format, args) < 0) {
        return -1;
    }
    console_flush(&out);
    if (out.error < 0) {
        return out.error;
    }
    return (int)out.total;
}

int
printf(const char *format, ...)
{
    int rc;
    va_list args;

    va_start(args, format);
    rc = vprintf(format, args);
    va_end(args);
    return rc;
}
