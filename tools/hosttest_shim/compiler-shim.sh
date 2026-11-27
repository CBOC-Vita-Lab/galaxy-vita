#!/usr/bin/env bash
# Compiler shim put first on PATH by tools/run_host_tests.sh.  When a test script asks for
# -fsanitize=X and this machine's compiler cannot link that sanitizer runtime (e.g. clang without
# libclang_rt.asan), the flag is dropped so the test still builds and runs (result cached per flag).
# HOSTTEST_SAN=on keeps every flag untouched.
name=$(basename "$0")
real=$(PATH=${HOSTTEST_REAL_PATH:-$PATH} command -v "$name") || { echo "shim: no real $name" >&2; exit 127; }
[ "${HOSTTEST_SAN:-auto}" = on ] && exec "$real" "$@"
cache=${HOSTTEST_CACHE:-/tmp/hosttest-san-cache}; mkdir -p "$cache" 2>/dev/null
args=()
for a in "$@"; do
  case "$a" in
    -fsanitize=*)
      key="$cache/$name-$(printf %s "$a" | tr -c 'A-Za-z0-9' _)"
      if [ ! -f "$key" ]; then
        d=$(mktemp -d); echo 'int main(){return 0;}' >"$d/p.c"
        if "$real" -x c "$a" "$d/p.c" -o "$d/p" >/dev/null 2>&1 && "$d/p" >/dev/null 2>&1; then echo ok >"$key"; else echo no >"$key"; fi
        rm -rf "$d"
      fi
      [ "$(cat "$key")" = ok ] || continue ;;
  esac
  args+=("$a")
done
exec "$real" "${args[@]}"
