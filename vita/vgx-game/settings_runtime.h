#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Nonzero means leave before boot (Cancel/Exit or a failed cold adapter).
int port_settings_startup(int argc, char** argv);
// The input host calls this before any normal sample or publication.
int port_settings_input_service(void);
#ifdef __cplusplus
}
#endif
