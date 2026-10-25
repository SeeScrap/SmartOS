#include "kernel/kfmt.h"

#include <stdint.h>

// Appends one character if there is room, and always counts it.
static inline void put(char *buf, size_t n, size_t *len, char c)
{
    if (*len + 1 < n) buf[*len] = c;
    (*len)++;
}

static void put_field(char *buf, size_t n, size_t *len, const char *s, size_t slen,
                      int width, bool left, char pad)
{
    int fill = width > (int)slen ? width - (int)slen : 0;

    // Zero padding goes after a sign, never before it: -0042, not 00-42.
    if (!left && pad == '0' && slen > 0 && s[0] == '-') {
        put(buf, n, len, '-');
        ++s;
        --slen;
    }
    if (!left) while (fill-- > 0) put(buf, n, len, pad);
    for (size_t i = 0; i < slen; ++i) put(buf, n, len, s[i]);
    if (left)  while (fill-- > 0) put(buf, n, len, ' ');
}

int k_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t len = 0;

    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') { put(buf, n, &len, *p); continue; }

        ++p;
        bool left = false;
        char pad  = ' ';
        for (;; ++p) {
            if (*p == '-')      left = true;
            else if (*p == '0') pad  = '0';
            else break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        // Precision: for %s, the most characters taken from the string (stage
        // 4.5 - the log viewer cuts lines to the screen width with %-53.53s).
        int prec = -1;
        if (*p == '.') {
            ++p;
            prec = 0;
            while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        while (*p == 'l') ++p;

        char     tmp[12];
        size_t   tlen = 0;
        uint32_t u;

        switch (*p) {
            case 'd':
            case 'i': {
                int32_t v = va_arg(ap, int32_t);
                bool neg = v < 0;
                u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
                char rev[11];
                size_t r = 0;
                do { rev[r++] = (char)('0' + u % 10); u /= 10; } while (u);
                if (neg) tmp[tlen++] = '-';
                while (r) tmp[tlen++] = rev[--r];
                put_field(buf, n, &len, tmp, tlen, width, left, pad);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                u = va_arg(ap, uint32_t);
                const unsigned base = (*p == 'u') ? 10 : 16;
                const char *dig = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                char rev[11];
                size_t r = 0;
                do { rev[r++] = dig[u % base]; u /= base; } while (u);
                while (r) tmp[tlen++] = rev[--r];
                put_field(buf, n, &len, tmp, tlen, width, left, pad);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t sl = 0;
                while (s[sl] && (prec < 0 || sl < (size_t)prec)) ++sl;
                put_field(buf, n, &len, s, sl, width, left, ' ');
                break;
            }
            case 'c':
                tmp[0] = (char)va_arg(ap, int);
                put_field(buf, n, &len, tmp, 1, width, left, ' ');
                break;
            case '%':
                put(buf, n, &len, '%');
                break;
            case '\0':
                --p;                        // a lone '%' at the very end
                break;
            default:                        // unknown: print it as written
                put(buf, n, &len, '%');
                put(buf, n, &len, *p);
                break;
        }
    }

    if (n > 0) buf[len < n ? len : n - 1] = '\0';
    return (int)len;
}

int k_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = k_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
