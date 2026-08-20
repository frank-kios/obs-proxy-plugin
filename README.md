# obs-proxy

An OBS Studio plugin (Windows) that routes OBS network traffic through a **SOCKS5** proxy.

It works entirely from inside the `obs64.exe` process by:

1. **Hooking Winsock** (`connect` / `WSAConnect`) — tunnels the main process's
   outbound TCP (RTMP / RTMPS streaming) through the SOCKS5 server.
2. **Hooking `CreateProcessW`** — when OBS spawns the CEF browser subprocess
   (`obs-browser-page.exe`), it appends `--proxy-server=socks5://host:port` so
   **browser sources** also use the proxy (with remote DNS via Chromium).

## What is / isn't covered

- ✅ **RTMP / RTMPS streaming** (TCP `connect`) → SOCKS5.
- ✅ **Browser sources** → CEF `--proxy-server=socks5://...`.
- ⚠️ **SRT / WHIP (UDP)** → not proxied (SOCKS5 UDP-ASSOCIATE is not
  implemented). Use RTMP if you need proxying.
- ⚠️ **DNS for streaming**: librtmp resolves the hostname before `connect`, so
  the stream uses local DNS + SOCKS5-by-IP. Browser sources use remote DNS.

## Compatibility

Works with **OBS Studio 32.1 and newer**. The module reports libobs API
version `32.1.0`, and OBS loads any module whose version is ≤ the running
core's version — so one build covers 32.1 → latest. Only long-stable
libobs/frontend and Qt6 APIs are used, so it runs against the Qt6 that ships
with any of those OBS releases.

If you need to raise or lower the minimum, change this line in
`src/plugin-main.c`:

```c
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(32, 1, 0)
```

## Requirements

- Windows 10/11 (x64)
- Visual Studio 2026 (Desktop C++, `v145` toolset) — OBS master builds with VS 2026
- CMake 4.2.3+ (needed for the `Visual Studio 18 2026` generator), or the CMake bundled with VS 2026
- Windows SDK 26100
- OBS Studio dev files (libobs + headers). Either build OBS from source or use
  the OBS deps package referenced by
  [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

[MinHook](https://github.com/TsudaKageyu/minhook) is fetched automatically by CMake.

## Build

```powershell
cmake -B build -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_PREFIX_PATH="C:/path/to/obs/libobs"
cmake --build build --config Release
```

## Install

### Option A — Installer (recommended)

The build also produces a self-contained installer that embeds the plugin DLL
and locale, auto-detects your OBS install, and elevates via UAC:

```
build\out\obs-proxy-installer.exe
```

- Double-click it (accept the UAC prompt). It copies the plugin into your OBS
  install and the locale into place.
- Point it at a specific OBS folder:
  `obs-proxy-installer.exe "C:\Program Files\obs-studio"`
- Unattended install: `obs-proxy-installer.exe /silent`
- Uninstall: `obs-proxy-installer.exe /uninstall`

Close OBS before installing/uninstalling.

### Option B — Manual copy

1. Copy `build\out\obs-proxy.dll` →
   `C:\Program Files\obs-studio\obs-plugins\64bit\`
2. Copy `data\locale` →
   `C:\Program Files\obs-studio\data\obs-plugins\obs-proxy\locale\`

## Configure

### Quick toggle + proxy picker

Two buttons are added to the bottom of OBS's **Controls** dock (below *Settings*):

- **Enable Proxy** — toggles proxying on/off instantly (turns green / *Proxy: ON*).
- **Proxy: …** — opens a popup to choose how a proxy is selected from the list:
  - **Round-robin** — rotate to the next proxy on each connection.
  - **Random** — pick a random proxy per connection.
  - **Specific proxy** — pick one entry from the list (double-click or select + OK).

  The button label shows the current mode / selected proxy. Selections are
  saved and applied immediately.

### In OBS (full settings)

Open **Tools ▸ OBS SOCKS5 Proxy Settings…**. The dialog lets you set:

- Enable proxy / Proxy browser sources
- Use proxy list from URL, the list URL, and refresh interval
- A static/fallback proxy (host, port, auth)

Click **OK** to save and apply immediately (no restart needed), or
**Reload list now** to re-fetch the list right away.

### By file

1. Start OBS once. The plugin writes a default config to:
   `%APPDATA%\obs-studio\plugin_config\obs-proxy\obs-proxy.json`
2. Edit it:

```json
{
    "enabled": true,
    "proxy_browser": true,

    "use_proxy_list": true,
    "proxy_list_url": "",
    "proxy_list_refresh_sec": 300,

    "host": "127.0.0.1",
    "port": 1080,
    "use_auth": false,
    "username": "",
    "password": ""
}
```

3. Restart OBS. Check the log for `[obs-proxy] loaded ...` and
   `[obs-proxy] proxy list: N parsed, M resolved`.

### Config keys

| Key | Meaning |
|---|---|
| `enabled` | Master on/off for proxying. |
| `proxy_browser` | Also proxy CEF browser sources. |
| `use_proxy_list` | Fetch a proxy list from `proxy_list_url`. |
| `proxy_list_url` | URL returning one proxy per line. |
| `proxy_list_refresh_sec` | Re-fetch interval in seconds (`0` = fetch once). |
| `host`/`port`/`use_auth`/`username`/`password` | Static fallback proxy used when the list is off or empty. |

### Proxy list format

One proxy per line. Blank lines and `#` comments are ignored. Accepted forms:

```
user:pass@host:port
host:port:user:pass
host:port
socks5://user:pass@host:port
```

Entries are resolved and used **round-robin**, one per outgoing connection
(and one per browser subprocess). The list is fetched in a background thread,
so OBS startup isn't blocked; the download itself is never tunneled.

## Notes / caveats

- Settings can be changed live via the Tools-menu dialog (applied on OK).
  Editing the JSON directly is also supported; that path is read at startup.
- The proxy list is fetched over HTTPS via WinHTTP and refreshed on the
  configured interval.
- API hooking can trip anti-cheat / AV heuristics on some systems.
