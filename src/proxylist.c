#define WIN32_LEAN_AND_MEAN
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <obs-module.h>
#include "proxy-config.h"
#include "proxylist.h"

static HANDLE g_thread;
static HANDLE g_stop;
static volatile DWORD g_fetch_tid;

// --------------------------------------------------------------------------
// HTTP(S) GET via WinHTTP
// --------------------------------------------------------------------------
static bool http_get(const wchar_t *url, char **body_out, DWORD *len_out)
{
    *body_out = NULL;
    *len_out = 0;

    URL_COMPONENTS uc;
    wchar_t host[256], path[1536];
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;   uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return false;

    HINTERNET hSession = WinHttpOpen(L"obs-proxy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return false;

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (hConnect) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq) {
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {

                DWORD cap = 8192, len = 0;
                char *buf = (char *)malloc(cap);
                if (buf) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                        if (len + avail + 1 > cap) {
                            while (len + avail + 1 > cap) cap *= 2;
                            char *nb = (char *)realloc(buf, cap);
                            if (!nb) { free(buf); buf = NULL; break; }
                            buf = nb;
                        }
                        DWORD got = 0;
                        if (!WinHttpReadData(hReq, buf + len, avail, &got) || got == 0)
                            break;
                        len += got;
                    }
                    if (buf) {
                        buf[len] = '\0';
                        *body_out = buf;
                        *len_out = len;
                        ok = true;
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

// --------------------------------------------------------------------------
// Parsing
// --------------------------------------------------------------------------
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

// Accepts: host:port | host:port:user:pass | user:pass@host:port,
// optionally prefixed with socks5:// (or any scheme://).
static bool parse_entry(char *line, struct proxy_entry *e)
{
    memset(e, 0, sizeof(*e));
    char *p = trim(line);
    if (!*p || *p == '#')
        return false;

    char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;

    char user[256] = "", pass[256] = "", host[256] = "", port[16] = "";

    char *at = strchr(p, '@');
    if (at) {
        *at = '\0';
        char *creds = p;
        char *hp = at + 1;
        char *colon = strchr(creds, ':');
        if (colon) {
            *colon = '\0';
            strncpy_s(user, sizeof(user), creds, _TRUNCATE);
            strncpy_s(pass, sizeof(pass), colon + 1, _TRUNCATE);
        } else {
            strncpy_s(user, sizeof(user), creds, _TRUNCATE);
        }
        char *c2 = strrchr(hp, ':');
        if (!c2) return false;
        *c2 = '\0';
        strncpy_s(host, sizeof(host), hp, _TRUNCATE);
        strncpy_s(port, sizeof(port), c2 + 1, _TRUNCATE);
    } else {
        char *tok[6];
        int n = 0;
        char *ctx = NULL;
        char *t = strtok_s(p, ":", &ctx);
        while (t && n < 6) { tok[n++] = t; t = strtok_s(NULL, ":", &ctx); }
        if (n == 2) {
            strncpy_s(host, sizeof(host), tok[0], _TRUNCATE);
            strncpy_s(port, sizeof(port), tok[1], _TRUNCATE);
        } else if (n >= 4) {
            strncpy_s(host, sizeof(host), tok[0], _TRUNCATE);
            strncpy_s(port, sizeof(port), tok[1], _TRUNCATE);
            strncpy_s(user, sizeof(user), tok[2], _TRUNCATE);
            strncpy_s(pass, sizeof(pass), tok[3], _TRUNCATE);
        } else {
            return false;
        }
    }

    int pn = atoi(port);
    if (pn <= 0 || pn > 65535 || !host[0])
        return false;

    strncpy_s(e->host, sizeof(e->host), host, _TRUNCATE);
    e->port = (uint16_t)pn;
    strncpy_s(e->username, sizeof(e->username), user, _TRUNCATE);
    strncpy_s(e->password, sizeof(e->password), pass, _TRUNCATE);
    e->use_auth = (user[0] != '\0');
    return true;
}

static void resolve_entry(struct proxy_entry *e)
{
    e->resolved_ok = false;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    sprintf_s(ps, sizeof(ps), "%u", (unsigned)e->port);
    if (getaddrinfo(e->host, ps, &hints, &res) == 0 && res) {
        memcpy(&e->resolved, res->ai_addr, sizeof(struct sockaddr_in));
        e->resolved_ok = true;
        freeaddrinfo(res);
    }
}

static void build_list(char *body)
{
    struct proxy_entry *arr = NULL;
    int cap = 0, cnt = 0, resolved = 0;

    char *ctx = NULL;
    char *line = strtok_s(body, "\n", &ctx);
    while (line) {
        struct proxy_entry e;
        if (parse_entry(line, &e)) {
            resolve_entry(&e);
            if (e.resolved_ok) resolved++;
            if (cnt == cap) {
                cap = cap ? cap * 2 : 16;
                struct proxy_entry *na = (struct proxy_entry *)realloc(arr, cap * sizeof(*arr));
                if (!na) break;
                arr = na;
            }
            arr[cnt++] = e;
        }
        line = strtok_s(NULL, "\n", &ctx);
    }

    EnterCriticalSection(&g_cfg.lock);
    free(g_cfg.list);
    g_cfg.list = arr;
    g_cfg.list_count = cnt;
    LeaveCriticalSection(&g_cfg.lock);

    blog(LOG_INFO, "[obs-proxy] proxy list: %d parsed, %d resolved", cnt, resolved);
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------
bool proxy_pick(struct proxy_entry *out)
{
    bool ok = false;
    if (!g_cfg.lock_ready) return false;

    EnterCriticalSection(&g_cfg.lock);
    if (g_cfg.use_list && g_cfg.list_count > 0) {
        int cnt = g_cfg.list_count;

        // Manual: use the chosen entry if it is resolved.
        if (g_cfg.select_mode == PROXY_SELECT_MANUAL) {
            int idx = g_cfg.selected_index;
            if (idx >= 0 && idx < cnt && g_cfg.list[idx].resolved_ok) {
                *out = g_cfg.list[idx];
                ok = true;
            }
        }

        // Round-robin / random (also the fallback if a manual pick is bad).
        if (!ok) {
            unsigned start;
            if (g_cfg.select_mode == PROXY_SELECT_RANDOM) {
                unsigned r = 0;
                if (rand_s(&r) != 0) r = (unsigned)GetTickCount();
                start = r;
            } else {
                start = (unsigned)InterlockedIncrement(&g_cfg.rr_index);
            }
            for (int n = 0; n < cnt; n++) {
                struct proxy_entry *e = &g_cfg.list[(start + n) % cnt];
                if (e->resolved_ok) { *out = *e; ok = true; break; }
            }
        }
    }
    if (!ok && g_cfg.single.resolved_ok) { *out = g_cfg.single; ok = true; }
    LeaveCriticalSection(&g_cfg.lock);
    return ok;
}

bool proxy_is_known(const struct sockaddr_in *dst)
{
    bool known = false;
    if (!g_cfg.lock_ready) return false;

    EnterCriticalSection(&g_cfg.lock);
    if (g_cfg.single.resolved_ok &&
        dst->sin_addr.s_addr == g_cfg.single.resolved.sin_addr.s_addr &&
        dst->sin_port == g_cfg.single.resolved.sin_port)
        known = true;
    for (int i = 0; !known && i < g_cfg.list_count; i++) {
        struct proxy_entry *e = &g_cfg.list[i];
        if (e->resolved_ok &&
            dst->sin_addr.s_addr == e->resolved.sin_addr.s_addr &&
            dst->sin_port == e->resolved.sin_port)
            known = true;
    }
    LeaveCriticalSection(&g_cfg.lock);
    return known;
}

bool proxylist_bypass_current_thread(void)
{
    DWORD t = g_fetch_tid;
    return t != 0 && GetCurrentThreadId() == t;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    wchar_t wurl[1024];
    MultiByteToWideChar(CP_UTF8, 0, g_cfg.list_url, -1, wurl, ARRAYSIZE(wurl));

    for (;;) {
        char *body = NULL;
        DWORD len = 0;

        g_fetch_tid = GetCurrentThreadId();
        bool ok = http_get(wurl, &body, &len);
        if (ok && body) {
            build_list(body);
            free(body);
        } else {
            blog(LOG_WARNING, "[obs-proxy] failed to fetch proxy list from %s",
                 g_cfg.list_url);
        }
        g_fetch_tid = 0;

        if (g_cfg.refresh_sec <= 0)
            break;
        if (WaitForSingleObject(g_stop, (DWORD)g_cfg.refresh_sec * 1000) != WAIT_TIMEOUT)
            break; // stop signaled
    }
    return 0;
}

void proxylist_start(void)
{
    if (!g_cfg.use_list || !g_cfg.list_url[0])
        return;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

void proxylist_stop(void)
{
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = NULL;
    }
}
