#pragma once

// Opt-in quiet builds preserve game clocks and pacing, but omit diagnostic work.
#ifndef PORT_RUNTIME_DIAGNOSTICS
#define PORT_RUNTIME_DIAGNOSTICS 1
#endif

// Silent player builds also remove sparse summaries and the file-writer thread.
#ifndef PORT_LOGGING
#define PORT_LOGGING 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Sparse performance summaries bypass verbose logging and go only to the log buffer.
void port_log_summary(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
