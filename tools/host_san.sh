# Sourced by host test scripts (bash 3.2+). host_san <compiler> <sanitizer flags...> prints the flags, one per line, when
# <compiler> can build, link and run a trivial program with them here, and prints nothing (plus one note on stderr) when it cannot,
# for example clang without its ASan runtime on a Linux host. The test is then built plain and still runs; only the sanitizer
# leg is lost, and the note says so.
#   HOST_SANITIZE=1  require the sanitizers (fail when they do not work)
#   HOST_SANITIZE=0  never use them
#   unset            use them when they work (default)
#   use:  san=($(host_san "$cxx" -fsanitize=address,undefined -fno-omit-frame-pointer)); "$cxx" ... ${san[@]+"${san[@]}"} ...
host_san() {
    local cxx=$1; shift
    if [[ ${HOST_SANITIZE:-} == 0 ]]; then return 0; fi
    local d; d=$(mktemp -d)
    printf 'int main() { return 0; }\n' > "$d/t.cpp"
    local extra=() f
    if [[ $(basename "$cxx") == clang* ]]; then   # (clang without its own runtime: clang's instrumentation + GCC's libasan / libubsan, as tools/host_cxx.sh does)
        extra=(-fno-sanitize-link-runtime -Wl,--no-as-needed)
        for f in "$@"; do case "$f" in -fsanitize=*address*) extra+=(-l:libasan.so.8);; esac; done
        for f in "$@"; do case "$f" in -fsanitize=*undefined*) extra+=(-l:libubsan.so.1);; esac; done
    fi
    if "$cxx" "$@" "$d/t.cpp" -o "$d/t" > /dev/null 2>&1 && "$d/t" > /dev/null 2>&1; then
        printf '%s\n' "$@"
    elif [[ ${#extra[@]} -gt 0 ]] && "$cxx" "$@" "$d/t.cpp" -o "$d/t" "${extra[@]}" > /dev/null 2>&1 && "$d/t" > /dev/null 2>&1; then
        printf '%s\n' "$@" "${extra[@]}"
    elif [[ ${HOST_SANITIZE:-} == 1 ]]; then
        echo "host_san: $cxx cannot build with $* and HOST_SANITIZE=1" >&2; rm -rf "$d"; return 1
    else
        echo "host_san: $cxx cannot build with $* here; building WITHOUT sanitizers (HOST_SANITIZE=1 makes this an error)" >&2
    fi
    rm -rf "$d"
}

# host_backend_cxx: the compiler for a test that compiles the vgx back end (platform/vgx/src/vgx_backend_gl.cpp). g++ 13 refuses its two extern "C"
# port_scene_serial declarations (an "overloaded function" error: pre-existing, the Vita compiler is clang), so this is clang++ when there is one,
# whatever CXX says (tools/run_host_tests.sh exports CXX=g++); BACKEND_CXX overrides; with no clang++ it falls back to CXX / g++ and says so on stderr.
# The back end builds without sanitizers in every test (the GL stub, not the code under test, is most of it).
host_backend_cxx() {
    if [[ -n ${BACKEND_CXX:-} ]]; then printf '%s\n' "$BACKEND_CXX"
    elif command -v clang++ > /dev/null 2>&1; then printf 'clang++\n'
    else echo "host_backend_cxx: no clang++: the back end builds with ${CXX:-g++}, which may refuse port_scene_serial" >&2; printf '%s\n' "${CXX:-g++}"; fi
}
# host_pick_cxx: a compiler that has the sanitizer runtime: clang++ first, else g++, else clang++ / g++ plain (one note on stderr: the suite still runs, WITHOUT them).
host_pick_cxx() {
    local c
    for c in clang++ g++; do
        command -v "$c" > /dev/null 2>&1 || continue
        if [[ -n $(host_san "$c" -fsanitize=address,undefined 2> /dev/null) ]]; then printf '%s\n' "$c"; return 0; fi
    done
    echo "host_pick_cxx: no compiler here has the ASan/UBSan runtime: building WITHOUT sanitizers" >&2
    if command -v clang++ > /dev/null 2>&1; then printf 'clang++\n'; else printf 'g++\n'; fi
}
