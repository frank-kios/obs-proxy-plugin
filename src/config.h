#pragma once

// Writes the current g_cfg to the plugin's JSON config file.
void obsproxy_save_config(void);

// Applies g_cfg to the running plugin: re-resolves the static proxy and
// restarts the proxy-list fetch thread with the current settings.
void obsproxy_apply_runtime(void);
