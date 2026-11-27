# Isolated macOS Vita3K checkpoint experiment

This is a patch to **Vita3K**, not a Vita game change. Its only target is
upstream `92bda548f3aa0691102471a72725587e00676383`, the source revision of
the archived Mac test binary (build 4232). It is **off by default**, and
none of these files go in the VPK. Do not edit the installed emulator or use
your normal user or shader directories for this experiment.

The original serializer runs inside swap. The opt-in path takes a full
immutable snapshot there: shader hash bytes, pipeline keys, and the Vulkan
driver blob. Only bounded filesystem work moves off the calling thread. It
does not change Vulkan calls to create/compile/use pipelines, shader or
disk-cache coverage, graphics settings, game code, or the 15-second debounce.
It requires memory for at most one in-flight full snapshot. A busy period
retries the next second, *before* allocating another. Shutdown drains the
older worker and saves the latest snapshot synchronously after the render
thread and pipeline compilers join, before destroying the driver cache. Each cache file is
written via a unique temporary in its own directory, flushed, then renamed;
two files are not one cross-file transaction. A failed replacement leaves
the prior file in place. A forced process kill can lose the latest pending
checkpoint and leave an unreferenced temporary, not a partially written .dat.

## Host verification

```sh
sh tools/emu_hitch/test.sh
# Additional check against a local checkout at the exact pin, without editing it:
sh tools/emu_hitch/test.sh /path/to/clean-or-modified-pinned-vita3k
```

The test extracts the actual header from the shipped patch, verifies it against
the standalone readable copy, compiles it, and exercises exact native byte order
for both file formats, a 2 MiB driver payload, injected partial/exceptional
writes, empty driver data, bounded worker lifetime and shutdown ordering. No
private game files, GPU, VitaSDK, or emulator are needed.

## Isolated native A/B

From this game source root, on an Apple Silicon Mac with Xcode, CMake 3.31.10,
Ninja 1.13.2, Qt 6.11.0 (including Svg, Multimedia, LinguistTools) and OpenSSL
static development libraries available:

```sh
patch="$PWD/tools/emu_hitch/vita3k-92bda548.patch"
lab="$(mktemp -d "${TMPDIR:-/tmp}/vita3k-checkpoint-build.XXXXXXXX")"
git clone --no-checkout https://github.com/Vita3K/Vita3K.git "$lab/src"
cd "$lab/src"
git checkout --detach 92bda548f3aa0691102471a72725587e00676383
git submodule update --init --recursive
git apply --check "$patch"
git apply "$patch"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
: "${Qt6_ROOT:?set this to the Qt 6.11.0 macos SDK directory}"
: "${OPENSSL_PREFIX:?set this to the OpenSSL development prefix}"
cmake -S . -B native-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DUSE_LTO=NEVER -DVITA3K_FORCE_CUSTOM_BOOST=ON \
    -DQt6_ROOT="$Qt6_ROOT" -DUSE_DISCORD_RICH_PRESENCE=OFF -DFORCE_BUILD_OPENSSL_MAC=OFF \
    -DOPENSSL_INCLUDE_DIR="$OPENSSL_PREFIX/include" \
    -DOPENSSL_SSL_LIBRARY="$OPENSSL_PREFIX/lib/libssl.a" \
    -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_PREFIX/lib/libcrypto.a"
cmake --build native-build --target Vita3K --parallel 4
```

Use the **same isolated newly built binary** in both arms. Baseline: leave
`VITA3K_ASYNC_PIPELINE_CHECKPOINT` unset (or set it to `0`). Treatment: set
`VITA3K_ASYNC_PIPELINE_CHECKPOINT=1` in the **Mac launcher process**, not
the game's env.txt. No installed Vita3K or VPK understands this switch.
Before each run, restore an identical disposable user/cache/save clone;
hold route, driver, game build, config and launch options fixed. Alternate
order. Record full guest hitch/present data and host log in both arms. In
treatment, `Pipeline checkpoint snapshot:` reports retained caller cost;
`Pipeline checkpoint disk write:` reports write duration and success.
Close normally; verify the next launch loads the cache. Fail on an unreadable
cache, failed write, or persistent frames coincident with snapshotting; do not
claim the whole hitch was fixed when Vulkan snapshot work remains.

**Build receipt:** The full arm64 executable linked on macOS with Xcode Apple
Clang 17, the macOS 15.5 SDK, the upstream source pin above and its complete
recursive submodule set. The full commit ancestry yields build 4232-92bda548.
The build used the upstream MoltenVK 1.4.1 and ffmpeg-core prebuilt from
submodule revision `671860a3d70b1fa60b988fc725cebbebef729367`. The only
non-system dynamic dependencies in the executable are seven Qt 6.11 frameworks,
resolved relative to the app. Build-host OpenSSL is statically linked. The
patched executable is a lab input, not part of the game package.

| Artifact | SHA-256 on this host |
| --- | --- |
| Emulator source patch | `4ff9f49e924588147c955fb35a56973876eb4f16741da36fec2eb6ed59a9a8ac` |
| Local built executable, before copying into test app | `9ced7a1b4df37838ace875660b418915c991631b2b0f9b188524b54b5a003c11` |
| Upstream MoltenVK-macos.tar | `5ea0c259df7ded9a275444820f09cced54d6e5a7c7a31d262de62a5cdb7e15cf` |
| Upstream FFmpeg prebuilt zip | `c1213e7b4ab5abe79ac9d9e772f7b2646d1dd012e16c6ccd9f38e0f50440de28` |

The worker test, exact-source patch check and full native link passed.
Neither these checks nor native emulator frame times are physical Vita evidence.
