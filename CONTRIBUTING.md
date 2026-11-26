# Contributing to BeloAPI

Thanks for helping. BeloAPI is deliberately one file, and it stays that way:
routes, tools, JSON and the HTTP server all live in `server.cpp`.

The biggest open task is **first-class Linux and macOS support**. Windows is the
only platform with a published binary today; the POSIX code path builds and runs
but needs maintainers who use it daily.

## TL;DR

```bash
g++ -std=c++17 -O2 -Wall -Wextra -o beloapi server.cpp -pthread
./beloapi --test          # all nine routes must PASS
./beloapi --port 8787 &   # then exercise every route + /mcp
```

Open a pull request against `main` with a short description of what changed and
the test output. Never include `client.cpp` or `recording.json` in a commit.

## What to work on

| Area | Status | Notes |
| --- | --- | --- |
| Windows build | Released | WinHTTP client, static `.exe` via CI |
| Linux build | Builds from source | Uses the `curl` CLI for HTTPS |
| macOS build | Builds from source | Same as Linux; not yet tested by a maintainer |
| Native libcurl client | Wanted | Removes the `curl` binary dependency |
| More endpoints | Wanted | Listing detail, experiences, services |
| Tests | Wanted | A script that drives the running server |

### Porting the HTTP client

`http_request()` is the only platform-specific function. The `#else` branch
executes `curl` through `popen`, which works but costs a process per request.

To add a native implementation:

1. Add `#if defined(BELOAPI_USE_LIBCURL)` around a libcurl implementation of
   `http_request`.
2. Keep the exact signature and the `ApiResult` contract: transport failures set
   `error` and leave `status` at 0; HTTP errors keep their status and body.
3. Add the definition to `CMakeLists.txt` behind an option, defaulting to off.
4. Document the flag in `README.md` and here.

Anything that changes the observable response format is a breaking change and
needs a version bump.

## Adding an HTTP route

1. Capture the real request from airbnb.com (DevTools → Network → *Copy as
   cURL*) while logged out. Record the method, path, query parameters and body.
2. Write a handler in section 6 of `server.cpp`:

   ```cpp
   inline ApiResult handle_thing(const Params& p) {
       std::string value = pick(p, {"thing", "q"});
       if (value.empty())
           return local_error(400, "missing_parameter", "Please pass ?thing=...");
       // Build the upstream request and return the ApiResult unchanged.
   }
   ```

3. Register it in `route()`.
4. Add it to the endpoint table in `README.md` and to `index_document()`.
5. Add a `Case` to `run_self_test()` so `--test` covers it.

Handlers must reuse `upstream_get`, `upstream_post`, `graphql_get` or
`graphql_post`, which set the shared headers. Never add a cookie, a password or a
user token.

## Adding an MCP tool

1. Add an entry to `tool_table()` in section 7: name, description, JSON Schema,
   and the handler pointer. Reuse an existing handler whenever possible.
2. Prefix the name with `airbnb_`.
3. List it in the README tool table.
4. Verify it with:

   ```bash
   curl -s -X POST http://127.0.0.1:8787/mcp -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"airbnb_thing","arguments":{}}}'
   ```

## Style

- C++17, standard library only. No dependencies, no submodules, no build-time
  code generation.
- Section banners are part of the documentation; keep the numbering consistent.
- Comments explain *why*. A function that only forwards parameters does not need
  a paragraph restating its signature.
- Prefer `std::string` and `std::vector`; the code avoids owning raw pointers.
- Keep the server loopback-only. A feature that binds a public interface will not
  be merged.
- Never log cookies, tokens or personal data.

## Test checklist for a pull request

```bash
# 1. Build clean
g++ -std=c++17 -O2 -Wall -Wextra -static -o beloapi.exe server.cpp -lwinhttp -lws2_32

# 2. Upstream routes
./beloapi.exe --test

# 3. Live server
./beloapi.exe --port 8787 &
curl -s http://127.0.0.1:8787/api/health
curl -s "http://127.0.0.1:8787/api/autocomplete?query=Nairobi"
curl -s "http://127.0.0.1:8787/api/search?location=Mombasa&adults=2&items=3"
for p in markets homepage announcements promotions header consent config; do
  curl -s -o /dev/null -w "$p %{http_code}\n" "http://127.0.0.1:8787/api/$p"
done

# 4. MCP handshake
curl -s -X POST http://127.0.0.1:8787/mcp -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
curl -s -X POST http://127.0.0.1:8787/mcp -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

Paste the results in the pull request. A change that breaks `--test` is not
ready.

## Reporting problems

Open an issue with the failing route, the exact request, the status code and the
first lines of the response. Do not paste cookies or personal data.

For anything that should stay private, email **crackboyshop@proton.me**.

## License

Contributions are accepted under the [MIT license](LICENSE). By opening a pull
request you confirm you wrote the code and can license it under those terms.
