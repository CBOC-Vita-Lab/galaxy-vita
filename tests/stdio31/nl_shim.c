/* stdio31 host proof: the environment the VitaSDK newlib stdio objects need, compiled with the SDK's own GCC (so struct FILE is the SDK's
 * layout). tools/test_stdio31.sh links this with fvwrite.o fflush.o fwrite.o fputs.o wbuf.o wsetup.o makebuf.o putc.o, taken unchanged from
 * $VITASDK/arm-vita-eabi/lib/libc.a, into an armhf Linux program run under qemu-arm. Every newlib name in these objects is renamed nl_<name>
 * (objcopy --redefine-syms) so that glibc's own stdio never meets them. */
#include <reent.h>
#include <stdio.h>
#include <string.h>

void* malloc(size_t);  /* (glibc's: these objects call _malloc_r, defined here) */
void free(void*);
void* realloc(void*, size_t);

static struct _reent shim_reent; /* one for every thread: the objects only read __cleanup (non-zero: no __sinit) and store _errno */
struct _reent* __getreent(void) { return &shim_reent; }
void __sinit(struct _reent* r) { (void)r; }
struct _reent _impure_data;
struct _glue __sglue;
void* _malloc_r(struct _reent* r, size_t n) { (void)r; return malloc(n); }
void _free_r(struct _reent* r, void* p) { (void)r; free(p); }
void* _realloc_r(struct _reent* r, void* p, size_t n) { (void)r; return realloc(p, n); }
int _fstat_r(struct _reent* r, int fd, struct stat* st) { (void)r; (void)fd; (void)st; return -1; }
int _isatty_r(struct _reent* r, int fd) { (void)r; (void)fd; return 0; }

static FILE* shim_files[4];
static int shim_nfiles;
/* fflush(NULL): newlib walks every FILE (here: the ones made below) */
int _fwalk_sglue(struct _reent* r, int (*fn)(struct _reent*, FILE*), struct _glue* g) {
    (void)g;
    int ret = 0;
    for (int i = 0; i < shim_nfiles; i++) ret |= fn(r, shim_files[i]);
    return ret;
}

typedef int (*shim_write_fn)(void* cookie, const char* data, int n);
static shim_write_fn shim_write_cb;
static int shim_write(struct _reent* r, void* cookie, const char* data, int n) {
    (void)r;
    return shim_write_cb(cookie, data, n);
}

/* A write stream as setvbuf(f, buf, mode, size) leaves it (mode 0 = _IOFBF, 1 = _IOLBF, 2 = _IONBF; buf unused for _IONBF). */
FILE* shim_make_file(int mode, unsigned char* buf, int size, void* cookie, shim_write_fn cb) {
    static FILE files[4];
    if (shim_nfiles >= 4) return 0;
    shim_reent.__cleanup = (void (*)(struct _reent*))1;
    shim_write_cb = cb;
    FILE* f = &files[shim_nfiles];
    memset(f, 0, sizeof *f);
    f->_file = 2;
    f->_cookie = cookie;
    f->_write = shim_write;
    f->_flags = __SWR;
    if (mode == 2) {
        f->_flags |= __SNBF;
        f->_bf._base = f->_p = f->_nbuf;
        f->_bf._size = 1;
    } else {
        if (mode == 1) f->_flags |= __SLBF;
        f->_bf._base = f->_p = buf;
        f->_bf._size = size;
    }
    f->_w = 0;  /* (setvbuf's state: __swsetup_r sets _w / _lbfsize at the first write) */
    f->_lbfsize = 0;
    shim_files[shim_nfiles++] = f;
    return f;
}
/* For the test's checks: the FILE's write state. */
void shim_file_state(FILE* f, unsigned char** p, int* w, unsigned char** base, int* size) {
    *p = f->_p;
    *w = f->_w;
    *base = f->_bf._base;
    *size = f->_bf._size;
}
int shim_sizeof_file(void) { return (int)sizeof(FILE); }
