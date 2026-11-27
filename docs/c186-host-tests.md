# C186 host regression suite

From the checkout root, run **`bash tools/test_c186.sh`**. No game dump, save,
shader seed, Vita, emulator, account or public test server is required. The
test programs use generated data and temporary directories; the HTTPS test
starts a loopback-only fixture with a temporary test CA. The suite does not
install, deploy or contact a game service.

**Linux x86-64:** install Bash, Clang/Clang++, GNU C++ headers, ASan/UBSan
runtimes, GNU make, Git, GNU binutils, LLVM including `llvm-profdata`, Node.js,
Python 3 with `cryptography`, and Mesa EGL/GLES2 development libraries and the
software GL driver (llvmpipe). The target uses a real TLS implementation,
not a simulated network when testing timeouts and socket lifetimes.

Prepare the public TLS dependency once if it is not already installed. Keep
the Linux checkout and build paths free of spaces (TLS tool requirement):

```sh
python3 -I tools/mp/build-mbedtls.py --target host64 --work /tmp/galaxy-host-deps
export MBEDTLS=/tmp/galaxy-host-deps/mbedtls/host64
bash tools/test_c186.sh
```

That **dependency preparation** may fetch the public checkout; the host tests
are offline. The committed lock (`net/deps/lock.json`) pins Mbed TLS to
`068ff080b369adfac81509f9b57b2afabaf82dc5`, including the project's patch
and config. Alternatively set `C186_TLS_ARCHIVE` to the handoff's pinned
`mbedtls-068ff080-checkout.tgz`; the suite verifies its digest, extracts into
scratch, and builds the host library automatically. It contains no game data.

**macOS / Docker:** the same command uses the locally retained
`cboc-strikers-12-final-tests:20261031` x86-64 host tools image (no pull)
and the verified TLS archive, with networking disabled except loopback and
the source mounted read-only. Set `C186_TLS_ARCHIVE` to its absolute path
when not using the original workspace layout. An equivalent local image can
be selected via `C186_HOST_IMAGE`. A clean Linux machine does not require
this image or the archive: use the public Linux prerequisites above.

The full command includes the C183 regression chain, retail-profile guard,
C184 exactness tests, C186 diagnostic-switch gate, first-visit queue and
actual GL backend test, generated GLSL pixels, Vita format scanner, Settings
and menu restart model, simulated online transport and loopback HTTPS suite.
For a partial native host run, `bash tools/test_c186.sh core` excludes the
Linux-only integration cases. Neither a passing host suite nor a shorter
run establishes console FPS, an online hardware pass or the vibration's cause.
