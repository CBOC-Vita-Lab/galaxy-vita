# Small archive reads

C183's first Honeyhive scene build used 166 synchronous disc requests for 83
files (83 separate 32-byte compression probes). Game code also resolved every
filename three times: existence, length, and open. The C184 ripper looks up
the entry once and gets the cooked length from the same file descriptor used
for the read. This does not rely on a filename extension or a compression flag
supplied by the converter.

For a rounded input length of at most 32 KiB, the loader tries to borrow its
existing decompression window without waiting. A raw file is then read,
classified and copied to the final game allocation in one worker trip. Both
heap directions and caller-owned destinations are preserved. Publication of
the final bytes includes the normal graphics-cache notification. Larger files
and a busy/missing window keep the original two-read route. For alternate
compressed game data, release the window before using the existing decoder;
up to 32 KiB can be re-read for this case. No new persistent buffer, cache or
file on the memory card. Set `GXV_ARCHIVE_BATCH=0` to disable window borrowing.

The compression probe used to retain a pointer to a block-local stack array
after the block ended. Its 32-byte aligned payload now lives for the whole load.
Empty compressed-input requests are rejected before subtracting the header size.

The host test executes the production function and streamed decoder with
counted DVD operations under ASan/UBSan. It checks returned bytes, EOF padding,
caller and heap storage, publish range, cancellation, retry, contention,
allocation failure, sizes around the cutoff, and Yaz0/Yay0. Replaying 307
distinct raw cooked archives opened by the C183 Honeyhive route (including
startup) needed **430 vs. 614** worker trips and 1,860,224 copied bytes. It is
not a physical Vita timing, and that total is not a claim about one scene load.
