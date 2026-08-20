#include <obs-module.h>
#include <util/platform.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include "proxy-config.h"
#include "hooks.h"
#include "proxylist.h"
#include "config.h"
#include "settings-ui.h"
#include "dock.h"

/* Report the minimum supported libobs version (OBS 32.1) so a single build
 * loads on OBS 32.1 and every newer release. OBS refuses to load modules whose
 * reported version is greater than the running core's version (see
 * libobs/obs-module.c: `if (ver > LIBOBS_API_VER) ... MODULE_INCOMPATIBLE_VER`).
 * We only use long-stable libobs/frontend APIs, so this is safe. */
#undef LIBOBS_API_VER
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(32, 1, 0)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-proxy", "en-US")

struct proxy_config g_cfg = {0};

static void resolve_single(void)
{
    g_cfg.single.resolved_ok = false;
    if (!g_cfg.single.host[0] || g_cfg.single.port == 0)
        return;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)g_cfg.single.port);
    if (getaddrinfo(g_cfg.single.host, portstr, &hints, &res) == 0 && res) {
        memcpy(&g_cfg.single.resolved, res->ai_addr, sizeof(struct sockaddr_in));
        g_cfg.single.resolved_ok = true;
        freeaddrinfo(res);
    }
}

static void set_defaults(void)
{
    g_cfg.enabled = false;
    g_cfg.proxy_browser = true;
    g_cfg.use_list = true;
    g_cfg.list_url[0] = '\0';
    g_cfg.refresh_sec = 300;
    g_cfg.select_mode = PROXY_SELECT_ROUNDROBIN;
    g_cfg.selected_index = -1;
    snprintf(g_cfg.single.host, sizeof(g_cfg.single.host), "%s", "127.0.0.1");
    g_cfg.single.port = 1080;
    g_cfg.single.use_auth = false;
    g_cfg.single.username[0] = '\0';
    g_cfg.single.password[0] = '\0';
}

void obsproxy_save_config(void)
{
    char *dir = obs_module_config_path(NULL);
    if (dir) { os_mkdirs(dir); bfree(dir); }
    char *path = obs_module_config_path("obs-proxy.json");
    if (!path) return;

    obs_data_t *d = obs_data_create();
    obs_data_set_bool(d, "enabled", g_cfg.enabled);
    obs_data_set_bool(d, "proxy_browser", g_cfg.proxy_browser);
    obs_data_set_bool(d, "use_proxy_list", g_cfg.use_list);
    obs_data_set_string(d, "proxy_list_url", g_cfg.list_url);
    obs_data_set_int(d, "proxy_list_refresh_sec", g_cfg.refresh_sec);
    obs_data_set_int(d, "select_mode", g_cfg.select_mode);
    obs_data_set_int(d, "selected_index", g_cfg.selected_index);
    obs_data_set_string(d, "host", g_cfg.single.host);
    obs_data_set_int(d, "port", g_cfg.single.port);
    obs_data_set_bool(d, "use_auth", g_cfg.single.use_auth);
    obs_data_set_string(d, "username", g_cfg.single.username);
    obs_data_set_string(d, "password", g_cfg.single.password);
    obs_data_save_json(d, path);
    obs_data_release(d);
    bfree(path);
}

void obsproxy_apply_runtime(void)
{
    resolve_single();
    proxylist_stop();
    if (g_cfg.use_list)
        proxylist_start();
    blog(LOG_INFO,
         "[obs-proxy] settings applied (enabled=%d browser=%d list=%d url=%s)",
         g_cfg.enabled, g_cfg.proxy_browser, g_cfg.use_list, g_cfg.list_url);
}

static void load_config(void)
{
    char *dir = obs_module_config_path(NULL);
    if (dir) { os_mkdirs(dir); bfree(dir); }

    char *path = obs_module_config_path("obs-proxy.json");
    obs_data_t *d = path ? obs_data_create_from_json_file(path) : NULL;

    if (!d) {
        set_defaults();
        obsproxy_save_config();
    } else {
        g_cfg.enabled       = obs_data_get_bool(d, "enabled");
        g_cfg.proxy_browser = obs_data_get_bool(d, "proxy_browser");
        g_cfg.use_list      = obs_data_get_bool(d, "use_proxy_list");
        snprintf(g_cfg.list_url, sizeof(g_cfg.list_url), "%s",
                 obs_data_get_string(d, "proxy_list_url"));
        g_cfg.refresh_sec = (int)obs_data_get_int(d, "proxy_list_refresh_sec");
        g_cfg.select_mode = (int)obs_data_get_int(d, "select_mode");
        obs_data_set_default_int(d, "selected_index", -1);
        g_cfg.selected_index = (int)obs_data_get_int(d, "selected_index");
        snprintf(g_cfg.single.host, sizeof(g_cfg.single.host), "%s",
                 obs_data_get_string(d, "host"));
        g_cfg.single.port     = (uint16_t)obs_data_get_int(d, "port");
        g_cfg.single.use_auth = obs_data_get_bool(d, "use_auth");
        snprintf(g_cfg.single.username, sizeof(g_cfg.single.username), "%s",
                 obs_data_get_string(d, "username"));
        snprintf(g_cfg.single.password, sizeof(g_cfg.single.password), "%s",
                 obs_data_get_string(d, "password"));
        obs_data_release(d);
    }
    if (path) bfree(path);
    resolve_single();
}

bool obs_module_load(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    InitializeCriticalSection(&g_cfg.lock);
    g_cfg.lock_ready = true;

    load_config();

    if (!hooks_install()) {
        blog(LOG_ERROR, "[obs-proxy] failed to install hooks");
        return true; // stay loaded so config still works after fix
    }

    if (g_cfg.use_list)
        proxylist_start();

    settings_ui_register();
    proxy_dock_register();

    blog(LOG_INFO,
         "[obs-proxy] loaded (enabled=%d browser=%d list=%d url=%s refresh=%ds)",
         g_cfg.enabled, g_cfg.proxy_browser, g_cfg.use_list,
         g_cfg.list_url, g_cfg.refresh_sec);
    return true;
}

void obs_module_unload(void)
{
    proxy_dock_unregister();
    proxylist_stop();
    hooks_remove();

    if (g_cfg.lock_ready) {
        EnterCriticalSection(&g_cfg.lock);
        free(g_cfg.list);
        g_cfg.list = NULL;
        g_cfg.list_count = 0;
        LeaveCriticalSection(&g_cfg.lock);
        DeleteCriticalSection(&g_cfg.lock);
        g_cfg.lock_ready = false;
    }

    WSACleanup();
    blog(LOG_INFO, "[obs-proxy] unloaded");
}
