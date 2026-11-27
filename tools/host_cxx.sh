#!/bin/bash
# host_cxx.sh: clang++ that can LINK -fsanitize=address/undefined on a host where clang has no sanitizer runtime (libclang_rt.asan is missing on this builder).
# The instrumentation is clang's, the runtime is GCC's libasan / libubsan (the same pairing tools/test_vgx_shader_list.sh's docker image uses:
# -fno-sanitize-link-runtime -l:libasan.so.8 -l:libubsan.so.1). Used as CXX by tools/run_all_host_tests.sh, so the ~200 scripts that hard-code `-fsanitize=...`
# get a working sanitizer build with the compiler the decomp code needs (g++ 13 rejects it) while each script keeps its own compiler choice. Without the missing runtime this is plain clang++ (exec).
# HOST_SANITIZE=0 drops every -fsanitize flag (the suite builds plain, and the runner says so); BACKEND_CXX etc. are not involved.
# Installed as `clang++` / `clang` in a shim directory at the FRONT of PATH by tools/run_all_host_tests.sh (HOST_CXX_REAL_DIR = where the real ones are), or used as CXX.
case "$(basename "$0")" in clang|clang++) real=${HOST_CXX_REAL_DIR:-/usr/bin}/$(basename "$0");; *) real=${HOST_CXX_REAL:-clang++};; esac
[[ ${1:-} == --probe ]] && { d=$(mktemp -d); printf 'int main(){return 0;}\n' > "$d/t.cpp"; "$0" -fsanitize=address,undefined "$d/t.cpp" -o "$d/t" >/dev/null 2>&1 && "$d/t" >/dev/null 2>&1; rc=$?; rm -rf "$d"; exit $rc; }
args=("$@"); san=0; asan=0; ubsan=0; compile_only=0; shared=0; out=()
for a in "${args[@]}"; do
  case "$a" in
    -fsanitize=*) v=${a#-fsanitize=}
        if [[ ${HOST_SANITIZE:-} == 0 ]]; then continue; fi
        case ",$v," in *,thread,*|*,memory,*|*,fuzzer*|*,hwaddress,*) ;; *) san=1; case ",$v," in *,address,*) asan=1;; esac; case ",$v," in *,undefined,*) ubsan=1;; esac;; esac;;
    -c|-S|-E) compile_only=1;;
    -shared) shared=1;;
  esac
  out+=("$a")
done
need_fix=0
if [[ $san == 1 && $compile_only == 0 && $shared == 0 ]]; then
  if [[ -z ${HOST_CXX_NATIVE_SAN:-} ]]; then
    d=$(mktemp -d); printf 'int main(){return 0;}\n' > "$d/t.cpp"
    if "$real" -fsanitize=address,undefined "$d/t.cpp" -o "$d/t" >/dev/null 2>&1 && "$d/t" >/dev/null 2>&1; then HOST_CXX_NATIVE_SAN=1; else HOST_CXX_NATIVE_SAN=0; fi
    rm -rf "$d"
  fi
  [[ $HOST_CXX_NATIVE_SAN == 0 ]] && need_fix=1
fi
if [[ $need_fix == 1 ]]; then
  extra=(-fno-sanitize-link-runtime -Wl,--no-as-needed)
  [[ $asan == 1 ]] && extra+=(-l:libasan.so.8)
  [[ $ubsan == 1 ]] && extra+=(-l:libubsan.so.1)
  exec "$real" "${out[@]}" "${extra[@]}"
fi
exec "$real" "${out[@]}"
