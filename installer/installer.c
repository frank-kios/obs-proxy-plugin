#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include "resource.h"

static int write_resource(int id, const wchar_t *path)
{
    HRSRC h = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return -1;
    HGLOBAL g = LoadResource(NULL, h);
    if (!g) return -1;
    void *data = LockResource(g);
    DWORD size = SizeofResource(NULL, h);
    if (!data || size == 0) return -1;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, NULL);
    CloseHandle(f);
    return (ok && written == size) ? 0 : -1;
}

static BOOL detect_obs(wchar_t *out, DWORD cch)
{
    DWORD sz = cch * (DWORD)sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS Studio",
            L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            NULL, out, &sz) == ERROR_SUCCESS && out[0])
        return TRUE;

    wchar_t pf[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf) == S_OK) {
        swprintf(out, cch, L"%s\\obs-studio", pf);
        if (PathFileExistsW(out)) return TRUE;
    }
    return FALSE;
}

static void pause_if_interactive(BOOL silent)
{
    if (!silent) {
        wprintf(L"\nPress Enter to exit...");
        (void)getwchar();
    }
}

int wmain(int argc, wchar_t **argv)
{
    BOOL uninstall = FALSE, silent = FALSE;
    wchar_t obs[MAX_PATH] = {0};

    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/uninstall") || !_wcsicmp(argv[i], L"-u"))
            uninstall = TRUE;
        else if (!_wcsicmp(argv[i], L"/silent") || !_wcsicmp(argv[i], L"-s"))
            silent = TRUE;
        else
            wcscpy_s(obs, MAX_PATH, argv[i]);
    }

    wprintf(L"obs-proxy installer\n===================\n\n");

    if (!obs[0] && !detect_obs(obs, MAX_PATH)) {
        wprintf(L"Could not locate OBS Studio.\n");
        wprintf(L"Run again with the OBS folder, e.g.:\n");
        wprintf(L"  obs-proxy-installer.exe \"C:\\Program Files\\obs-studio\"\n");
        pause_if_interactive(silent);
        return 1;
    }

    wchar_t pluginDir[MAX_PATH], localeDir[MAX_PATH];
    wchar_t dllPath[MAX_PATH], iniPath[MAX_PATH], proxyData[MAX_PATH];
    swprintf(pluginDir, MAX_PATH, L"%s\\obs-plugins\\64bit", obs);
    swprintf(localeDir, MAX_PATH, L"%s\\data\\obs-plugins\\obs-proxy\\locale", obs);
    swprintf(dllPath, MAX_PATH, L"%s\\obs-proxy.dll", pluginDir);
    swprintf(iniPath, MAX_PATH, L"%s\\en-US.ini", localeDir);
    swprintf(proxyData, MAX_PATH, L"%s\\data\\obs-plugins\\obs-proxy", obs);

    wprintf(L"OBS folder: %s\n\n", obs);

    if (uninstall) {
        DeleteFileW(dllPath);
        DeleteFileW(iniPath);
        RemoveDirectoryW(localeDir);
        RemoveDirectoryW(proxyData);
        wprintf(L"Uninstalled obs-proxy.\n");
        pause_if_interactive(silent);
        return 0;
    }

    SHCreateDirectoryExW(NULL, pluginDir, NULL);
    SHCreateDirectoryExW(NULL, localeDir, NULL);

    if (write_resource(IDR_PLUGIN_DLL, dllPath) != 0) {
        wprintf(L"ERROR: failed to write %s\n", dllPath);
        wprintf(L"(Run as administrator, and make sure OBS is closed.)\n");
        pause_if_interactive(silent);
        return 1;
    }
    if (write_resource(IDR_LOCALE_INI, iniPath) != 0) {
        wprintf(L"ERROR: failed to write %s\n", iniPath);
        pause_if_interactive(silent);
        return 1;
    }

    wprintf(L"Installed:\n  %s\n  %s\n\n", dllPath, iniPath);
    wprintf(L"Next: start OBS once, then edit the config and set \"enabled\": true:\n");
    wprintf(L"  %%APPDATA%%\\obs-studio\\plugin_config\\obs-proxy\\obs-proxy.json\n");
    pause_if_interactive(silent);
    return 0;
}
