# BeloAPI setup guide

This guide covers building, running, keeping BeloAPI alive in the background and
connecting an MCP client. For the endpoint list see [../README.md](../README.md).

BeloAPI is a console program. It prints one line per request and keeps running
until you stop it. It listens on **127.0.0.1:8787** and never on a public
interface.

## 1. Prerequisites

| Platform | What you need |
| --- | --- |
| Windows (prebuilt) | Nothing. `beloapi.exe` is self-contained. |
| Windows (source) | MinGW-w64 GCC 9+ or Visual Studio 2019+ |
| Linux | `g++` 9+ and `curl` (used for outbound HTTPS) |
| macOS | Xcode command line tools, `curl` (ships with macOS) |

`curl` is required on Linux and macOS because the server delegates outbound TLS
to it. Windows uses WinHTTP and needs nothing extra.

## 2. Build

### Windows with MinGW

```bat
g++ -std=c++17 -O2 -static -o beloapi.exe server.cpp -lwinhttp -lws2_32
```

### Windows with MSVC

From a "x64 Native Tools Command Prompt":

```bat
cl /std:c++17 /O2 /EHsc server.cpp /link winhttp.lib ws2_32.lib
```

### Linux

```bash
g++ -std=c++17 -O2 -o beloapi server.cpp -pthread
```

### macOS

```bash
g++ -std=c++17 -O2 -o beloapi server.cpp -pthread
```

### CMake (any platform)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The binary lands in `build/` (or `build/Release/` with multi-config generators).

## 3. Run

```bat
beloapi.exe
```

```
  BeloAPI v1.2.0 - unofficial Airbnb API + MCP
  by Dennis Nganga Mburu (CrackBoy), Kenya
  contact: crackboyshop@proton.me

  http://127.0.0.1:8787/           landing page
  http://127.0.0.1:8787/api/health health
  http://127.0.0.1:8787/mcp        MCP endpoint (POST)

  Ctrl+C to stop.
```

Options:

| Flag | Meaning |
| --- | --- |
| `--port <n>` | Listen on another port (default 8787) |
| `--mcp-stdio` | Serve MCP over stdin/stdout instead of HTTP (for desktop AI clients) |
| `--host <addr>` | Address to bind (default `127.0.0.1`); non-loopback needs `--allow-public` |
| `--allow-public` | Permit binding beyond loopback, for server use |
| `--token <value>` | Require this token on `/api` and `/mcp` (`BELOAPI_TOKEN` also works) |
| `--test` | Call every upstream route once and print PASS/FAIL |
| `--version` | Print the version |
| `--help` | Print usage |

### Check that it works

```bash
curl "http://127.0.0.1:8787/api/health"
curl "http://127.0.0.1:8787/api/autocomplete?query=Mombasa"
curl "http://127.0.0.1:8787/api/search?location=Mombasa&adults=2"
```

A full self test that does not start the server:

```bash
beloapi --test
```

It should print eleven `PASS` lines, ending with `mcp tools/list` and
`openapi.json`.

## 4. Run in the background

BeloAPI has no daemon mode; use the operating system's own mechanism so a crash
or logout is handled properly.

### Windows — current session

```bat
start "" /b beloapi.exe > beloapi.log 2>&1
```

To stop it:

```bat
taskkill /IM beloapi.exe /F
```

### Windows — start with the machine (Task Scheduler)

1. Task Scheduler → **Create Task**.
2. General: *Run whether user is logged on or not*.
3. Triggers: *At startup*.
4. Actions: start `C:\path\to\beloapi.exe`, arguments `--port 8787`,
   *Start in* `C:\path\to`.
5. Settings: *Restart the task if it fails*, 1 minute.

### Linux — systemd user service

`~/.config/systemd/user/beloapi.service`:

```ini
[Unit]
Description=BeloAPI unofficial Airbnb API
After=network-online.target

[Service]
ExecStart=%h/bin/beloapi --port 8787
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now beloapi
journalctl --user -u beloapi -f
```

### macOS — launchd

`~/Library/LaunchAgents/me.crackboy.beloapi.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>me.crackboy.beloapi</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/beloapi</string>
    <string>--port</string><string>8787</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict>
</plist>
```

```bash
launchctl load ~/Library/LaunchAgents/me.crackboy.beloapi.plist
```

### Remote access

The server refuses to bind anything except loopback. If you need it from
another machine, put it behind a reverse proxy (Caddy, nginx, Tailscale) and add
authentication there. Never expose it directly to the internet: it is an open
proxy to a third-party service.

## 5. Connect an MCP client

If you expose BeloAPI to other machines, start it with `--allow-public --host
0.0.0.0` and a token, then read [API.md](API.md#8-deployment) for the nginx,
systemd and service setup. The rest of this section is about MCP clients.

Two transports are available. README has the full examples; these are the two
that matter.

**stdio** — the client starts the binary. No server needs to be running, and no
Node bridge is involved. Claude Desktop (`claude_desktop_config.json`), Cursor
or VS Code:

```json
{
  "mcpServers": {
    "beloapi": {
      "command": "C:\\path\\to\\beloapi.exe",
      "args": ["--mcp-stdio"]
    }
  }
}
```

**HTTP** — start BeloAPI first, then point the client at
`http://127.0.0.1:8787/mcp`.

```json
{ "mcpServers": { "beloapi": { "url": "http://127.0.0.1:8787/mcp" } } }
```

**Codex CLI** — `~/.codex/config.toml`:

```toml
[mcp_servers.beloapi]
url = "http://127.0.0.1:8787/mcp"
```

**VS Code / Copilot** — `.vscode/mcp.json`:

```json
{ "servers": { "beloapi": { "type": "http", "url": "http://127.0.0.1:8787/mcp" } } }
```

**Bridging** — only needed if a stdio-only client cannot be pointed at an
executable directly:

```bash
npx -y mcp-remote http://127.0.0.1:8787/mcp
```

and register that command as the server in the client's config.

Supported protocol methods: `initialize`, `notifications/initialized`, `ping`,
`tools/list`, `tools/call`. Protocol version `2024-11-05` (newer clients
negotiate down to it).

## 6. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `Could not bind 127.0.0.1:8787` | Another process owns the port. Run with `--port 9000`. |
| `[FAIL] ... status=400` in `--test` | Airbnb rejected a persisted-query hash. See below. |
| `curl: command not found` (Linux/macOS) | Install `curl`; the POSIX build uses it for HTTPS. |
| `status=502` from an endpoint | No internet, a proxy, or a firewall blocking `www.airbnb.com`. |
| MCP client shows no tools | Start the server first, then reconnect the client; confirm with `curl -X POST .../mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'`. |
| macOS blocks the binary | `xattr -d com.apple.quarantine ./beloapi` after downloading a build. |

### When Airbnb changes a query

The GraphQL routes use persisted-query hashes captured from the live site. If
Airbnb deploys a new bundle the old hash can start returning
`{"errors":[{"message":"PersistedQueryNotFound"...}]}`.

1. Open airbnb.com in a browser with DevTools on the Network tab.
2. Find the failing `/api/v3/<Operation>/<hash>` request.
3. Copy the new hash and, if the variables changed, the new request body.
4. Update the `kHash...` constant (and the variables in the handler) in
   `server.cpp`, rebuild, and run `beloapi --test`.

## 7. Before publishing a release

1. `beloapi --test` → all routes PASS.
2. Start the server and hit every `/api/*` route plus `/mcp` (see the checklist
   in [CONTRIBUTING.md](../CONTRIBUTING.md)).
3. Bump `kVersion` in `server.cpp` and `project(... VERSION ...)` in
   `CMakeLists.txt`, then commit.
4. Tag and push: `git tag v1.0.0 && git push origin v1.0.0`. The
   `Windows release` workflow builds `beloapi.exe` and attaches it to the GitHub
   release.
