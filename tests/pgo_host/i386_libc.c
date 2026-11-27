/* A minimal 32-bit x86 Linux "C library" (system calls by int 0x80, a bump allocator) so that the profile runtime and a test program run
 * as an ILP32 little-endian target like the Vita, on a host without 32-bit libraries.  Test code only. */
#include <stddef.h>
#include <stdint.h>

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
  return r;
}
long pgo_sys_open(const char *path) { return sys3(5, (long)path, 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644); }
long pgo_sys_write(int fd, const void *p, unsigned n) { return sys3(4, fd, (long)p, n); }
long pgo_sys_close(int fd) { return sys3(6, fd, 0, 0); }

typedef struct _FILE FILE;
FILE *stderr = 0;
int fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
char *getenv(const char *name) { (void)name; return 0; }
int atoi(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

static unsigned char heap[8 << 20];
static size_t heapUsed;
void *malloc(size_t n) { n = (n + 15) & ~(size_t)15; if (heapUsed + n > sizeof heap) return 0; void *p = heap + heapUsed; heapUsed += n; return p; }
void *calloc(size_t n, size_t m) { return malloc(n * m); }  /* (the heap is zero from the start and never reused) */
void free(void *p) { (void)p; }

void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }

int main(int argc, char **argv);
void pgo_start_c(long *sp) { /* sp -> argc, argv... */
  int argc = (int)sp[0];
  int r = main(argc, (char **)(sp + 1));
  sys3(1, r, 0, 0);
  for (;;) {}
}
__asm__(".globl _start\n_start:\n xor %ebp,%ebp\n mov %esp,%eax\n and $-16,%esp\n sub $12,%esp\n push %eax\n call pgo_start_c\n");
