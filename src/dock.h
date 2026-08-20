#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Registers a frontend callback that injects an "Enable Proxy" toggle button
// into OBS's Controls dock once the UI has finished loading.
void proxy_dock_register(void);
void proxy_dock_unregister(void);

#ifdef __cplusplus
}
#endif
