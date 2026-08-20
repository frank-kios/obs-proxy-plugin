#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "hooks.h"
#include "socks5.h"
#include "proxy-config.h"
#include "proxylist.h"
#include "MinHook.h"

typedef int (WSAAPI *connect_t)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wsaconnect_t)(SOCKET, const struct sockaddr *, int,
                                   LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef BOOL (WINAPI *createprocessw_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                        LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                        LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                        LPPROCESS_INFORMATION);

static connect_t        real_connect;
static wsaconnect_t     real_WSAConnect;
static createprocessw_t real_CreateProcessW;

static bool should_bypass(const struct sockaddr_in *dst)
{
    if (!g_cfg.enabled) return true;
    // never tunnel our own proxy-list download
    if (proxylist_bypass_current_thread()) return true;
    // don't proxy loopback
    if ((ntohl(dst->sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u) return true;
    // don't proxy a connection whose target is itself a proxy
    if (proxy_is_known(dst)) return true;
    return false;
}

// connect to the given proxy first, waiting for completion on non-blocking sockets
static int connect_to_proxy(SOCKET s, const struct sockaddr_in *paddr)
{
    int rc = real_connect(s, (const struct sockaddr *)paddr, sizeof(*paddr));
    if (rc == 0) return 0;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = { 10, 0 };
        if (select(0, NULL, &w, NULL, &tv) <= 0) return -1;
        int so_err = 0, len = sizeof(so_err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len);
        return so_err == 0 ? 0 : -1;
    }
    return -1;
}

static int tunnel(SOCKET s, const struct sockaddr *name)
{
    const struct sockaddr_in *dst = (const struct sockaddr_in *)name;
    struct proxy_entry px;
    if (!proxy_pick(&px)) {
        // no usable proxy yet (e.g. list not fetched) -> connect directly
        return real_connect(s, name, sizeof(struct sockaddr_in));
    }
    if (connect_to_proxy(s, &px.resolved) != 0) {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    if (socks5_handshake(s, &px, dst->sin_addr, dst->sin_port) != 0) {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return 0; // caller believes it reached the target directly
}

static int WSAAPI hooked_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    if (!name || name->sa_family != AF_INET ||
        should_bypass((const struct sockaddr_in *)name))
        return real_connect(s, name, namelen);
    return tunnel(s, name);
}

static int WSAAPI hooked_WSAConnect(SOCKET s, const struct sockaddr *name, int namelen,
                                    LPWSABUF cd, LPWSABUF ud, LPQOS sq, LPQOS gq)
{
    if (!name || name->sa_family != AF_INET ||
        should_bypass((const struct sockaddr_in *)name))
        return real_WSAConnect(s, name, namelen, cd, ud, sq, gq);
    return tunnel(s, name);
}

// Inject the CEF SOCKS5 switch into the browser-source subprocess.
static BOOL WINAPI hooked_CreateProcessW(LPCWSTR app, LPWSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD flags,
        LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    struct proxy_entry px;
    if (g_cfg.enabled && g_cfg.proxy_browser && cmd &&
        wcsstr(cmd, L"obs-browser-page") && proxy_pick(&px)) {
        wchar_t extra[400];
        swprintf(extra, 400, L" --proxy-server=socks5://%hs:%u",
                 px.host, (unsigned)px.port);
        size_t n = wcslen(cmd) + wcslen(extra) + 1;
        wchar_t *newcmd = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (newcmd) {
            wcscpy(newcmd, cmd);
            wcscat(newcmd, extra);
            BOOL r = real_CreateProcessW(app, newcmd, pa, ta, inh, flags, env, cwd, si, pi);
            free(newcmd);
            return r;
        }
    }
    return real_CreateProcessW(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
}

bool hooks_install(void)
{
    if (MH_Initialize() != MH_OK) return false;
    MH_CreateHookApi(L"ws2_32",   "connect",        &hooked_connect,        (void **)&real_connect);
    MH_CreateHookApi(L"ws2_32",   "WSAConnect",     &hooked_WSAConnect,     (void **)&real_WSAConnect);
    MH_CreateHookApi(L"kernel32", "CreateProcessW", &hooked_CreateProcessW, (void **)&real_CreateProcessW);
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

void hooks_remove(void)
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
