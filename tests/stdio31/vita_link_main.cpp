// stdio31 (tools/test_stdio31.sh): a tiny Vita program linked with the game's console wraps, to check that every console call of the link
// reaches a __wrap_ function and that both wrapper files (stdio_safe.cpp, silent_stdio.cpp) link against the SDK's C library. Not run.
#include <cstdio>
extern "C" void port_console_attach_writer(void);
extern "C" void port_console_drain(void);
extern "C" void port_console_configure(void);
int main() {
    port_console_configure();
    std::printf("a %d\n", 1); std::fprintf(stderr, "b %s\n", "x"); std::puts("c"); std::fputs("d\n", stderr); std::fputc('e', stderr); std::putchar('\n');
    std::fwrite("f\n", 1, 2, stdout); std::fflush(stderr); std::fflush(nullptr);
    port_console_attach_writer(); std::fprintf(stderr, "g\n"); port_console_drain();
    if (FILE* f = std::fopen("ux0:data/x.txt", "w")) { std::fprintf(f, "file %d\n", 2); std::fflush(f); std::fclose(f); }
    return 0;
}
