<p align="center">
  <img src="assets/airbnb-logo.png" alt="Airbnb" width="96" height="96">
</p>

<h1 align="center">BeloAPI</h1>

<p align="center">
  <strong>Unofficial Airbnb JSON API + MCP server — one C++ file, zero dependencies.</strong>
</p>

<p align="center">
  <a href="https://github.com/DennisNgangaMburu/beloapi/actions/workflows/windows-release.yml"><img alt="Windows build" src="https://img.shields.io/github/actions/workflow/status/DennisNgangaMburu/beloapi/windows-release.yml?label=windows%20build"></a>
  <img alt="Version" src="https://img.shields.io/badge/version-1.2.0-blue">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey">
</p>

---

BeloAPI runs on your machine and turns the public web requests that airbnb.com
makes in the browser into a clean local API. The same functions are exposed a
second time as **MCP tools**, so an AI agent can search stays and look up places
without screen scraping.

> **Not affiliated with Airbnb, Inc.** BeloAPI is an independent, unofficial
> project. "Airbnb" and the Bélo logo are trademarks of Airbnb, Inc. Use it for
> personal and educational work, respect Airbnb's Terms of Service, and do not
> use it to build a competing booking service. Read [DISCLAIMER](#disclaimer).

## Features

- **JSON API** — stays search, place autocomplete, markets, homepage feed,
  announcements, promotions, header, consent flags and client config.
- **MCP server** — JSON-RPC 2.0 over HTTP **or stdio** with nine tools, so
  Claude Desktop, Claude Code, Cursor, Codex, VS Code and other MCP clients can
  call it directly, with no bridge process.
- **One file** — `server.cpp` is the entire server. No npm, no Python, no DLLs.
- **Anonymous** — no login, no cookies, no API keys of your own. It only reads
  public data.
- **Portable core** — Windows is the released binary; Linux and macOS build from
  the same source (see [Contributing](#contributing)).

## Quick start (Windows)

1. Download `beloapi.exe` from [Releases](../../releases).
2. Run it:

   ```bat
   beloapi.exe
   ```

3. The API is now at `http://127.0.0.1:8787`.

```bash
curl "http://127.0.0.1:8787/api/health"
curl "http://127.0.0.1:8787/api/autocomplete?query=Mombasa"
curl "http://127.0.0.1:8787/api/search?location=Mombasa&checkin=2026-12-10&checkout=2026-12-12&adults=2"
```

Use a different port with `beloapi.exe --port 9000`. Run `beloapi.exe --help` for
every option, or `beloapi.exe --test` to check every upstream route.

## Endpoints

The complete developer reference — every parameter, the fields to read out of
each document, the error catalogue, status codes, integration examples (Node,
Python, Go, PHP, browser) and deployment recipes — is **[docs/API.md](docs/API.md)**.
Import `http://127.0.0.1:8787/openapi.json` into Postman, Insomnia or Swagger to
generate a client.

Responses are the raw Airbnb JSON documents, passed through with their original
HTTP status. Errors produced by BeloAPI itself look like
`{"error":{"code":"...","message":"..."}}`.

| Method | Path | What it returns |
| --- | --- | --- |
| GET | `/` | Landing document with the endpoint list |
| GET | `/api/health` | Version, author, uptime |
| GET | `/api/autocomplete?query=` | Place and destination suggestions, including `place_id` |
| GET | `/api/search?location=&checkin=&checkout=&adults=` | Stays search results, prices and pagination |
| GET | `/api/markets` | Region/market mapping for the caller |
| GET | `/api/homepage` | Curated homepage feed (`?vertical=STAYS`) |
| GET | `/api/announcements` | Site-wide announcements |
| GET | `/api/promotions` | Promotion slots (`?placements=HOME_PAGE_POPOVER`) |
| GET | `/api/header` | Logged-out header menu structure |
| GET | `/api/consent` | Consent flags |
| GET | `/api/config` | Client configuration document |
| GET | `/openapi.json` | OpenAPI 3.1 description, importable into Postman, Insomnia or Swagger |
| POST | `/mcp` | MCP over JSON-RPC 2.0 |

Extra query parameters: `locale`, `currency`, `country`, `num_results`,
`place_id`, `children`, `infants`, `pets`, `items`.

`/api/search` resolves `location` to a `place_id` automatically. Pass `place_id`
yourself to skip that lookup.

## Running it for other apps

By default BeloAPI listens on loopback only, which is what you want on a
workstation. To serve it to a website, a phone app or another server, opt in and
protect it with a token:

```bash
beloapi.exe --allow-public --host 0.0.0.0 --token "a-long-random-string"
```

`BELOAPI_TOKEN` in the environment does the same thing, and keeps the value out
of the process list. Callers then send `Authorization: Bearer <token>` (or
`X-API-Key:`) on every `/api/*` and `/mcp` request; `/` and `/openapi.json` stay
readable. Deployment behind nginx with TLS, systemd units, Windows services and
rate limiting are covered in [docs/API.md](docs/API.md#8-deployment).

## MCP

BeloAPI speaks MCP two ways. Use whichever your client supports.

**stdio** — for Claude Desktop, Cursor, VS Code, LM Studio and most desktop
clients. The client launches the binary itself, so nothing needs to be running
first and no Node bridge is needed:

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

**HTTP** — for clients that connect to a URL. Start `beloapi.exe` first:

```json
{
  "mcpServers": {
    "beloapi": { "url": "http://127.0.0.1:8787/mcp" }
  }
}
```

Codex (`~/.codex/config.toml`):

```toml
[mcp_servers.beloapi]
url = "http://127.0.0.1:8787/mcp"
```

If a stdio-only client insists on a bridge instead of launching the binary
itself, `npx -y mcp-remote http://127.0.0.1:8787/mcp` still works.

| Tool | Purpose |
| --- | --- |
| `airbnb_autocomplete` | Turn a place name into suggestions and a `place_id` |
| `airbnb_search_stays` | Search stays for a destination and date range |
| `airbnb_markets` | Region/market mapping |
| `airbnb_homepage` | Curated homepage feed |
| `airbnb_announcements` | Site announcements |
| `airbnb_promotions` | Promotion slots |
| `airbnb_header` | Header menu structure |
| `airbnb_consent` | Consent flags |
| `airbnb_client_config` | Client configuration |

A raw handshake, if you want to see the protocol:

```bash
curl -s -X POST http://127.0.0.1:8787/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"airbnb_search_stays","arguments":{"location":"Diani","adults":2}}}'
```

## Build from source

```bash
# Windows (MinGW)
g++ -std=c++17 -O2 -static -o beloapi.exe server.cpp -lwinhttp -lws2_32

# Linux / macOS
g++ -std=c++17 -O2 -o beloapi server.cpp -pthread
```

CMake works too: `cmake -B build && cmake --build build --config Release`.
Full instructions for MSVC, background/autostart operation and troubleshooting
are in **[docs/SETUP.md](docs/SETUP.md)**.

## Project layout

```
server.cpp                 the whole server: HTTP API, MCP, HTTP client
CMakeLists.txt             cross-platform build
docs/API.md                developer reference: endpoints, errors, integration, deployment
docs/SETUP.md              install, run, autostart, troubleshoot
CONTRIBUTING.md            how to add routes/tools and port the client
.github/workflows/         builds the Windows exe on every tag
assets/                    site logo and favicon
```

The source is organised in numbered sections (configuration, JSON, HTTP client,
upstream calls, API routes, MCP, server loop, self test). Each section starts
with a short explanation of what it does and why.

## Contributing

Linux and macOS are **build-from-source** today and need maintainers. The one
platform-specific piece is the outbound HTTP client in `server.cpp`
(`#ifdef _WIN32` / `#else`): Windows uses WinHTTP, other platforms shell out to
`curl`. Replacing that shim with a native libcurl client is the highest-value
first contribution. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Disclaimer

BeloAPI is unofficial and unaffiliated with Airbnb, Inc. It sends the same
requests a browser would send, using the public web configuration that
airbnb.com serves to every visitor. Airbnb can change or remove any of these
endpoints at any time, and persisted-query hashes do expire. You are
responsible for how you use it and for complying with Airbnb's Terms of Service
and the laws that apply to you. Do not use it for scraping at scale, bypassing
access controls, or reselling Airbnb data.

## Author

Built by **Dennis Nganga Mburu** — also known as **CrackBoy** — in Kenya 🇰🇪.

- Email: [crackboyshop@proton.me](mailto:crackboyshop@proton.me)
- Issues and feature requests: use the GitHub issue tracker.

## License

[MIT](LICENSE) © 2026 Dennis Nganga Mburu (CrackBoy).
