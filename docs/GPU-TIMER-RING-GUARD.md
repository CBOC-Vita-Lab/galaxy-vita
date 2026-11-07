# GPU Timer Ring Guard

`GXV_GPU_TIMER_GUARD=1` enables a read-once guard against overwriting a GPU
timer record that the watcher still owns. The default is `0`; unset or `0`
retains the old path. This requires rebuilding the GPU_TIMERS vitaGL variant
with the matching `tools/vitagl/gpu_timers.py` recipe. An older driver remains
usable and logs that the requested guard is unavailable.

The baseline producer can wrap its 256-record metadata ring while the watcher
waits for a scene notification. Replacing the waiting record's fence can make
completion of an old scene certify newer, unfinished scenes. The fence also
protects uniform-buffer reuse, so this is a correctness issue, not just an
inaccurate timing sample.

With the guard enabled, a full ring leaves the owned records intact. Extra
scenes still submit through the existing hardware completion fence, without
waiting for the watcher or discarding rendering commands. Only their timing
records are dropped. Recording resumes after the watcher drains space.

After the first dropped record, GPU cost samples are treated as unknown until
restart, including outside target-30 mode. This avoids mistaking partial GPU
work for spare capacity. One basic-log message reports the overflow. Automatic
quality's other signals retain their existing behavior; this does not freeze
the renderer or promise a fixed resolution.

## Local Checks

```sh
bash tools/test_vgx_gpu_timer_ring.sh
GUARD=0 bash tools/test_vgx_gpu_timer_ring.sh
GXV_GPU_TIMER_GUARD=1 CXX=clang++ bash tools/test_vgx_autoq_clock.sh
GXV_GPU_TIMER_GUARD=0 CXX=clang++ bash tools/test_vgx_autoq_clock.sh
CXX=clang++ bash tools/test_vgx_autoq.sh
```

The ring test executes C extracted from the actual driver recipe under
ASan/UBSan. It forces the producer to run while the watcher is blocked, verifies
the guard, drains and resumes the ring, and checks that incomplete timing stays
invalid. The OFF oracle deliberately confirms the old premature-fence defect
when 256 or more new scenes arrive. This is a deterministic fault injection,
not proof that a physical console reached that backlog.

## Console Check

Use the same build, save, route, and other settings, restarting between
`GXV_GPU_TIMER_GUARD=0` and `1`. Confirm the requested guard is available in the
basic log. Include long sessions, water entries/exits, and scene transitions.
Record any overflow message, fatal message, crash, or visual difference and
retain the exact build hash. The candidate does not claim that the reported
hardware freeze was caused by this interleaving, nor any FPS gain. Native SDK
linking, ARM memory-order behavior, and device completion semantics still need
validation. Sequence-number wraparound is not covered by this change.
