// The online service the builds use when nothing else names one. Galaxy 3.0: https://galaxy.psvita.dev (a Worker custom domain), compiled in
// by cmake/MpBuildDefs.cmake (-DNP_ONLINE_URL=...; the Vita source stage and tools/vgx_build/build.py --online-url pass it on; "none" = no
// service). There is NO fallback to the old smg-online.*.workers.dev address (an account belongs to ONE server address; a fallback would
// silently create a second account). Overrides stay: SMG_ONLINE_URL, then `server` in mp.cfg. Never the Strikers service (msc-online):
// Galaxy must not reach it, not even by accident.
// (Strikers 1.3 defaulted to its own service on the Vita; NP_ONLINE_URL_NONE is still accepted and means the same as no definition.)
#ifndef NP_SERVICE_URL_H
#define NP_SERVICE_URL_H

#if defined(NP_ONLINE_URL_NONE) && defined(NP_ONLINE_URL)
#undef NP_ONLINE_URL
#endif

#endif  // NP_SERVICE_URL_H
