#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "proxy-config.h"
#include "proxylist.h"
#include "config.h"
#include "settings-ui.h"
#include "ui-resource.h"

static HINSTANCE this_hinstance(void)
{
    HMODULE h = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&this_hinstance, &h);
    return (HINSTANCE)h;
}

static void set_check(HWND dlg, int id, bool v)
{
    CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED);
}

static bool get_check(HWND dlg, int id)
{
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

static void set_text_utf8(HWND dlg, int id, const char *s)
{
    wchar_t w[1200];
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, ARRAYSIZE(w));
    SetDlgItemTextW(dlg, id, w);
}

static void get_text_utf8(HWND dlg, int id, char *out, int cch)
{
    wchar_t w[1200];
    GetDlgItemTextW(dlg, id, w, ARRAYSIZE(w));
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cch, NULL, NULL);
}

static void read_form(HWND dlg)
{
    BOOL ok = FALSE;
    g_cfg.enabled       = get_check(dlg, IDC_ENABLED);
    g_cfg.proxy_browser = get_check(dlg, IDC_BROWSER);
    g_cfg.use_list      = get_check(dlg, IDC_USELIST);
    get_text_utf8(dlg, IDC_LISTURL, g_cfg.list_url, sizeof(g_cfg.list_url));

    UINT r = GetDlgItemInt(dlg, IDC_REFRESH, &ok, FALSE);
    g_cfg.refresh_sec = ok ? (int)r : 0;

    get_text_utf8(dlg, IDC_HOST, g_cfg.single.host, sizeof(g_cfg.single.host));
    UINT p = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
    g_cfg.single.port = ok ? (uint16_t)p : 0;
    g_cfg.single.use_auth = get_check(dlg, IDC_USEAUTH);
    get_text_utf8(dlg, IDC_USER, g_cfg.single.username, sizeof(g_cfg.single.username));
    get_text_utf8(dlg, IDC_PASS, g_cfg.single.password, sizeof(g_cfg.single.password));
}

static INT_PTR CALLBACK dlgproc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        set_check(dlg, IDC_ENABLED, g_cfg.enabled);
        set_check(dlg, IDC_BROWSER, g_cfg.proxy_browser);
        set_check(dlg, IDC_USELIST, g_cfg.use_list);
        set_text_utf8(dlg, IDC_LISTURL, g_cfg.list_url);
        SetDlgItemInt(dlg, IDC_REFRESH, (UINT)g_cfg.refresh_sec, FALSE);
        set_text_utf8(dlg, IDC_HOST, g_cfg.single.host);
        SetDlgItemInt(dlg, IDC_PORT, (UINT)g_cfg.single.port, FALSE);
        set_check(dlg, IDC_USEAUTH, g_cfg.single.use_auth);
        set_text_utf8(dlg, IDC_USER, g_cfg.single.username);
        set_text_utf8(dlg, IDC_PASS, g_cfg.single.password);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            read_form(dlg);
            obsproxy_save_config();
            obsproxy_apply_runtime();
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_RELOAD:
            read_form(dlg);
            obsproxy_save_config();
            obsproxy_apply_runtime();
            MessageBoxW(dlg, L"Reloading proxy list in the background.",
                        L"OBS SOCKS5 Proxy", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void menu_cb(void *data)
{
    (void)data;
    HWND parent = (HWND)obs_frontend_get_main_window_handle();
    DialogBoxParamW(this_hinstance(), MAKEINTRESOURCEW(IDD_SETTINGS),
                    parent, dlgproc, 0);
}

void settings_ui_register(void)
{
    obs_frontend_add_tools_menu_item("OBS SOCKS5 Proxy Settings...",
                                     menu_cb, NULL);
}
