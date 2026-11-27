// Keep file I/O intact while discarding console diagnostics before formatting.
#include "port/diagnostics.h"
#include <cstdarg>
#include <cstdio>

#if defined(PORT_SILENT_STDIO_WRAP) || !PORT_LOGGING
namespace {
bool LogStream(FILE* stream) { return stream == stdout || stream == stderr; }
}
extern "C" {
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_putc(int, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __wrap_printf(const char*, ...) { return 0; }
int __wrap_vprintf(const char*, va_list) { return 0; }
int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
    return LogStream(stream) ? 0 : __real_vfprintf(stream, format, args);
}
__attribute__((format(printf, 2, 3))) int __wrap_fprintf(FILE* stream, const char* format, ...) {
    if (LogStream(stream)) return 0;
    va_list args;
    va_start(args, format);
    const int result = __real_vfprintf(stream, format, args);
    va_end(args);
    return result;
}
int __wrap_puts(const char*) { return 0; }
int __wrap_putchar(int value) { return static_cast<unsigned char>(value); }
int __wrap_fputs(const char* text, FILE* stream) {
    return LogStream(stream) ? 0 : __real_fputs(text, stream);
}
int __wrap_fputc(int value, FILE* stream) {
    return LogStream(stream) ? static_cast<unsigned char>(value) : __real_fputc(value, stream);
}
int __wrap_putc(int value, FILE* stream) {
    return LogStream(stream) ? static_cast<unsigned char>(value) : __real_putc(value, stream);
}
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    return LogStream(stream) ? (size ? count : 0) : __real_fwrite(data, size, count, stream);
}
// stdio31: the console streams are never written here, so there is nothing to flush; and fflush(NULL) never walks every FILE (the C library's
// walk would flush streams other threads read or write without a lock: this newlib has no FILE locks, platform/include/port/console_stdio.h).
// (only in the game's link, whose CMake wraps fflush and defines PORT_SILENT_STDIO_WRAP; the host tests that link this file wrap the rest only)
#if defined(PORT_SILENT_STDIO_WRAP)
int __real_fflush(FILE*);
int __wrap_fflush(FILE* stream) {
    return (!stream || LogStream(stream)) ? 0 : __real_fflush(stream);
}
#endif
}
#endif
