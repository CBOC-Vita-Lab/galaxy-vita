// Host tests of the session layer (tests/mp/build.sh) link MpCore without net/'s online stack: the glue's per-frame entry is a no-op.
extern "C" void mp_netglue_frame(void) {}
