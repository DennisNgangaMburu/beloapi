# BeloAPI API reference

BeloAPI has **two separate surfaces**. This document covers the first one; the
second has its own guide.

| Surface | Who it is for | Entry point |
| --- | --- | --- |
| **REST proxy** (this document) | Apps, websites, scripts, backends | `GET /api/*` |
| **MCP server** | AI agents and coding assistants | `POST /mcp` or `--mcp-stdio`, see [README](../README.md#mcp) |

BeloAPI is a **proxy**: it holds no data of its own. It forwards requests to
Airbnb's public web documents and returns their JSON unchanged, with the
upstream HTTP status. That means responses are as complete as the website's own
data, and as fragile as an unofficial integration can be — read
[Stability](#stability-and-limits) before shipping.

- **Base URL:** `http://127.0.0.1:8787`
- **Format:** JSON over HTTP/1.1, UTF-8
- **OpenAPI:** `GET /openapi.json` (import into Postman, Insomnia, Swagger UI)
- **No third-party libraries, no login, no API key of your own**

---

## 1. Running it

```bash
beloapi.exe                 # loopback only, port 8787 — the default
beloapi.exe --port 9000     # another port
beloapi.exe --help          # every option
beloapi.exe --test          # check every upstream route, then exit
```

The binary is a foreground process. Your app talks to it over HTTP, so it must
be running whenever your app is. On a workstation that means a terminal or a
scheduled task; on a server it means a service (see [Deployment](#8-deployment)).

---

## 2. Authentication

**By default there is none, and that is safe because it only listens on
`127.0.0.1`.** Every request from the same machine is allowed.

When you expose it beyond loopback (for a web app, a phone app or another
server) protect it with a token:

```bash
beloapi.exe --allow-public --host 0.0.0.0 --token "a-long-random-string"
```

`BELOAPI_TOKEN` in the environment does the same thing and is preferred on
servers, because command lines are visible in process listings.

When a token is configured, send it on every `/api/*` and `/mcp` request, as
either header:

```
Authorization: Bearer a-long-random-string
X-API-Key: a-long-random-string
```

`GET /` and `GET /openapi.json` stay public so integrators can read the contract
before they have credentials. CORS preflight (`OPTIONS`) is never challenged, as
browsers do not send credentials on preflight.

A missing or wrong token returns:

```http
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer realm="beloapi"
Content-Type: application/json; charset=utf-8

{"error":{"code":"unauthorized","message":"Missing or invalid token. Send Authorization: Bearer <token> or X-API-Key."}}
```

Tokens are compared in constant time and are never logged. The query string is
deliberately **not** accepted as a token location, because request lines are
logged.

---

## 3. CORS

Browser apps can call BeloAPI directly from another origin:

```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type, Accept, Authorization, X-API-Key, Mcp-Session-Id
Access-Control-Max-Age: 86400
```

An `OPTIONS` preflight returns `204` with no body. Two browser caveats:

- A page served over **https** calling **http://127.0.0.1** works in current
  Chrome and Firefox because loopback counts as a trustworthy origin. If a
  future browser release enforces Private Network Access restrictions, proxy
  through your own backend instead.
- A wildcard origin cannot be combined with cookies — BeloAPI does not use
  cookies, so this is a non-issue.

---

## 4. Conventions

**Parameters** are query-string parameters on `GET`. Unknown parameters are
ignored, so adding a harmless extra key will not fail a request.

| Parameter | Applies to | Meaning |
| --- | --- | --- |
| `locale` | all | Language for labels and text, e.g. `en`, `fr`, `sw` |
| `currency` | all | ISO 4217 code, e.g. `USD`, `KES`, `EUR` — changes real prices returned |
| `country` | autocomplete | ISO country code, e.g. `KE` |
| `checkin`, `checkout` | search | `YYYY-MM-DD`. `checkout` must be after `checkin` |
| `items` | search | Results per page (default `18`) |
| `num_results` | autocomplete | Number of suggestions (default `5`) |
| `place_id` | search | Google place id from autocomplete |

**Dates** are always `YYYY-MM-DD` and interpreted in the destination's local
time. A malformed date returns `400 invalid_parameter`.

**Responses** are the upstream documents. BeloAPI does not reshape them, so
field names come from Airbnb. Two consequences worth planning for:

- Prices are formatted **strings** (`"$352"`), not numbers. Parse them or use
  `qualifier` to know the period.
- The documents are large. See [sizes](#9-performance-and-limits).

**Errors produced by BeloAPI** always use this shape:

```json
{ "error": { "code": "missing_parameter", "message": "Please pass ?location=, for example ?location=Mombasa", "docs": "https://github.com/DennisNgangaMburu/beloapi" } }
```

`code` is stable; `message` is human-readable and may change wording. Some
errors carry `hint` instead of `docs`. Never branch on the message text.

---

## 5. Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Service index: name, version, endpoints, repository |
| GET | `/api/health` | Liveness and version — use for health checks |
| GET | `/api/autocomplete` | Turn a place name into suggestions and `place_id` |
| GET | `/api/search` | Search stays: listings, prices, ratings, coordinates |
| GET | `/api/markets` | Region and market mapping for the caller |
| GET | `/api/homepage` | Curated homepage feed |
| GET | `/api/announcements` | Site-wide announcements |
| GET | `/api/promotions` | Promotion slots |
| GET | `/api/header` | Logged-out header menu |
| GET | `/api/consent` | Consent flags |
| GET | `/api/config` | Client config (timestamp) |
| GET | `/openapi.json` | OpenAPI 3.1 description |
| POST | `/mcp` | MCP over JSON-RPC 2.0 (separate surface) |

A trailing slash is normalised: `/api/health/` behaves like `/api/health`.

### GET /api/health

No parameters. Use this for container and load-balancer health checks: it does
not call upstream, so it stays fast and honest about the process being alive.

```json
{
  "status": "ok",
  "name": "BeloAPI",
  "version": "1.2.0",
  "author": "Dennis Nganga Mburu (CrackBoy), Kenya",
  "uptime_seconds": 42,
  "upstream": "https://www.airbnb.com"
}
```

### GET /api/autocomplete

Turn free text into candidate destinations. Every term carries a `place_id`
that `/api/search` accepts, which avoids re-resolving the name on every search.

| Parameter | Required | Default | Notes |
| --- | --- | --- | --- |
| `query` | yes | — | Free text, e.g. `Diani` |
| `num_results` | no | 5 | How many suggestions |
| `locale`, `currency`, `country` | no | — | Localisation |

```bash
curl "http://127.0.0.1:8787/api/autocomplete?query=Diani&num_results=2"
```

The fields you will actually use:

| JSON path | Example | Meaning |
| --- | --- | --- |
| `autocomplete_terms[].display_name` | `"Diani Beach, Kwale"` | Label to show a user |
| `autocomplete_terms[].location.location_name` | `"Diani Beach"` | Place name only |
| `autocomplete_terms[].location.google_place_id` | `"ChIJfRALKj9GQBgRrTn0sDoGoK8"` | Google place id |
| `autocomplete_terms[].explore_search_params.place_id` | same id | Pass this to `/api/search` |
| `autocomplete_terms[].suggestion_type` | `"DESTINATION"` | Suggestion kind |

Response is roughly 6–8 KB. If a query matches nothing, `autocomplete_terms` is
an empty array — that is not an error.

### GET /api/search

The main endpoint. Requires a destination, given either as `location` or as
`place_id` (at least one; both is fine and `place_id` wins for precision).

| Parameter | Required | Default | Notes |
| --- | --- | --- | --- |
| `location` | one of | — | `Mombasa`, `Diani Beach` |
| `place_id` | one of | — | From `/api/autocomplete` |
| `checkin`, `checkout` | no | — | `YYYY-MM-DD`; omit for flexible dates |
| `adults` | no | 1 | Guests affect price |
| `children`, `infants` | no | 0 | |
| `pets` | no | 0 | `1` when travelling with pets |
| `items` | no | 18 | Results per page |
| `locale`, `currency` | no | — | Localisation and real currency conversion |

```bash
curl "http://127.0.0.1:8787/api/search?place_id=ChIJfRALKj9GQBgRrTn0sDoGoK8&checkin=2026-12-10&checkout=2026-12-12&adults=2&items=3&currency=KES"
```

Where the data lives:

| JSON path | Example | Meaning |
| --- | --- | --- |
| `data.presentation.staysSearch.results.searchResults[]` | array | The listings |
| `...searchResults[].title` | `"Villa in Diani Beach"` | Listing type + area |
| `...searchResults[].subtitle` | `"Private Pool Villa..."` | Listing headline |
| `...searchResults[].avgRatingLocalized` | `"4.74 (50)"` | Rating and review count |
| `...searchResults[].structuredDisplayPrice.primaryLine.price` | `"$352"` | **Total for the stay** |
| `...primaryLine.qualifier` | `"for 2 nights"` | Period the price covers |
| `...primaryLine.accessibilityLabel` | `"$352 for 2 nights"` | Ready-made label |
| `...searchResults[].demandStayListing.location.coordinate` | `{"latitude":-4.31,"longitude":39.57}` | Map coordinates |
| `...searchResults[].contextualPictures[].picture` | `https://a0.muscache.com/...` | Photo URLs |
| `...searchResults[].listingParamOverrides` | `{checkin, checkout, adults, ...}` | Inputs used for this price |
| `...searchResults[].badges[]` | array | e.g. guest favourite |
| `results.paginationInfo.pageCursors[]` | array | Cursors Airbnb computed |

Prices are **totals for the requested stay**, not nightly rates — divide by
nights to compare. `listingParamOverrides` confirms which dates and guests the
price was calculated for, which is the reliable way to label a price in your UI.

> **Pagination:** the document contains `pageCursors`, but BeloAPI exposes no
> cursor parameter yet. To see more results, raise `items`, or narrow the
> search. Do not build paging on those cursors.

Response is roughly 140–260 KB for `items=18`.

### GET /api/markets

No required parameters. Returns the market Airbnb maps your region to, plus the
token used for personalised suggestions.

```json
{
  "market": "Nairobi",
  "country_code": "KE",
  "satori_region_id": -1,
  "version": "1.2.0",
  "user_markets": [{ "market": "Nairobi", "country_code": "KE", "locale": "en", "config_token": "..." }]
}
```

Useful for choosing a sensible default currency and locale in your UI. About
440 bytes.

### GET /api/homepage

Optional `vertical` (`STAYS`, `EXPERIENCES`, `SERVICES`), plus `locale` and
`currency`. Returns the curated feed:

```
data.presentation.homepage.tabbedVerticalHomepage.feedSections...
```

Large (roughly 230–270 KB) and mostly presentation metadata. Use it for
destination carousels, not for listing search.

### GET /api/announcements

`data.presentation.naviAnnouncements[]` — site-wide notices. Currently often an
empty array, which is normal. About 150 bytes.

### GET /api/promotions

Optional `placements` (comma-separated, default `HOME_PAGE_POPOVER`).
`data.merchandisablePromotions.promotionsByPlacements[]`. Often empty when no
campaign is running. About 160 bytes.

### GET /api/header

`data.presentation.header.topLevelItemGroup.items[]` — the logged-out navigation
tree. `data.viewer` is `null` because BeloAPI is never signed in. About 3 KB.

### GET /api/consent

Optional `device_id`, `consent_id`. Returns
`data.presentation.showConsent.showConsent.categoryView` (a JSON-encoded string
of consent categories). About 900 bytes.

### GET /api/config

`{"time_stamp": 1795705826}` — the client configuration timestamp. 25 bytes.
Handy as a cheap "is upstream reachable" probe that still exercises Airbnb.

### GET /openapi.json

The full OpenAPI 3.1 contract: every path, parameter, response and the optional
bearer scheme. Import it directly:

- **Postman:** Import → Link → `http://127.0.0.1:8787/openapi.json`
- **Insomnia / Swagger UI / openapi-generator:** same URL

---

## 6. Error reference

Every BeloAPI error has `error.code`, `error.message` and usually `error.docs`
or `error.hint`.

| Status | `code` | When it happens | Fix |
| --- | --- | --- | --- |
| 400 | `missing_parameter` | `?query=` omitted on autocomplete, or neither `location` nor `place_id` on search | Add the parameter |
| 400 | `invalid_parameter` | Date not `YYYY-MM-DD`, or `checkout` not after `checkin` | Correct the date |
| 400 | `bad_request` | Malformed HTTP, or body larger than the 8 MB cap | Send a valid request |
| 401 | `unauthorized` | Token configured but missing or wrong | Send `Authorization: Bearer <token>` |
| 404 | `location_not_found` | Airbnb has no place matching the name | Use `/api/autocomplete` first and pass `place_id` |
| 404 | `not_found` | Unknown path | `GET /` lists every route |
| 405 | `method_not_allowed` | `PUT`/`DELETE`, or `GET /mcp` | Use `GET` for `/api`, `POST` for `/mcp` |
| 413 | `bad_request` | Headers over 64 KB | Trim headers |
| 500 | `internal_error` | Unexpected server fault, includes the reason | Report it with the request |
| 502 | `upstream_unreachable` | No network, or Airbnb refused the call | Check connectivity; retry with backoff |

Real bodies:

```json
{"error":{"code":"missing_parameter","message":"Please pass ?location=, for example ?location=Mombasa","docs":"https://github.com/DennisNgangaMburu/beloapi"}}
{"error":{"code":"location_not_found","message":"Airbnb has no place matching \"Zzzzqqqxx\"","docs":"https://github.com/DennisNgangaMburu/beloapi"}}
{"error":{"code":"not_found","message":"No route for GET /nope","hint":"GET / lists every endpoint"}}
{"error":{"code":"method_not_allowed","message":"Use GET for /api and POST for /mcp"}}
```

Handling rules that will keep your integration stable:

1. Treat **any** non-2xx as an error, but do not assume a body shape for
   upstream failures — pass-through statuses (e.g. Airbnb returning `400` for
   an unsupported combination) may carry Airbnb's own error document.
2. Retry `502` and network timeouts only, with backoff. Retrying `400`/`404`
   will never succeed.
3. Log `error.code`, not just the HTTP status — it tells you whether the caller
   or the network is at fault.

---

## 7. Integration examples

### cURL

```bash
curl -s "http://127.0.0.1:8787/api/autocomplete?query=Mombasa" | head -c 300
curl -s -H "Authorization: Bearer $BELOAPI_TOKEN" \
     "http://127.0.0.1:8787/api/search?location=Diani&checkin=2026-12-10&checkout=2026-12-12&adults=2"
```

### Node.js / TypeScript (18+)

```ts
const BASE = process.env.BELOAPI_URL ?? "http://127.0.0.1:8787";
const TOKEN = process.env.BELOAPI_TOKEN;

type SearchResult = {
  title: string;
  subtitle?: string;
  avgRatingLocalized?: string;
  structuredDisplayPrice?: { primaryLine?: { price?: string; qualifier?: string } };
  demandStayListing?: { location?: { coordinate?: { latitude: number; longitude: number } } };
};

async function searchStays(query: Record<string, string>) {
  const url = new URL("/api/search", BASE);
  for (const [k, v] of Object.entries(query)) url.searchParams.set(k, v);

  const res = await fetch(url, {
    headers: TOKEN ? { Authorization: `Bearer ${TOKEN}` } : {},
    signal: AbortSignal.timeout(30_000),
  });

  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(`BeloAPI ${res.status} ${body?.error?.code ?? "unknown"}: ${body?.error?.message ?? ""}`);
  }

  const doc = await res.json();
  return doc.data.presentation.staysSearch.results.searchResults as SearchResult[];
}
```

Use `AbortSignal.timeout` generously: a search legitimately takes 1–2 seconds.

### Python

```python
import os, requests

BASE = os.environ.get("BELOAPI_URL", "http://127.0.0.1:8787")
TOKEN = os.environ.get("BELOAPI_TOKEN")
HEADERS = {"Authorization": f"Bearer {TOKEN}"} if TOKEN else {}

r = requests.get(f"{BASE}/api/search",
                 params={"location": "Diani", "checkin": "2026-12-10",
                         "checkout": "2026-12-12", "adults": 2, "items": 5},
                 headers=HEADERS, timeout=30)
r.raise_for_status()
results = r.json()["data"]["presentation"]["staysSearch"]["results"]["searchResults"]

for item in results:
    price = item.get("structuredDisplayPrice", {}).get("primaryLine", {})
    print(item["title"], "|", price.get("price"), price.get("qualifier"), "|", item.get("avgRatingLocalized"))
```

### Go

```go
req, _ := http.NewRequest("GET", "http://127.0.0.1:8787/api/search?location=Diani&items=3", nil)
if token := os.Getenv("BELOAPI_TOKEN"); token != "" {
    req.Header.Set("Authorization", "Bearer "+token)
}
client := &http.Client{Timeout: 30 * time.Second}
resp, err := client.Do(req)
defer resp.Body.Close()
```

### Browser (React example)

```jsx
useEffect(() => {
  const ctrl = new AbortController();
  const url = new URL("http://127.0.0.1:8787/api/search");
  url.searchParams.set("location", place);
  url.searchParams.set("items", "12");

  fetch(url, { signal: ctrl.signal })
    .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`))))
    .then((doc) => setListings(doc.data.presentation.staysSearch.results.searchResults))
    .catch((e) => { if (e.name !== "AbortError") setError(e.message); });

  return () => ctrl.abort();
}, [place]);
```

CORS is already open, so no dev proxy is required. Never ship a server token to
a browser — if you expose BeloAPI publicly, put your own backend in front and
keep the token there.

### Server-to-server

```
my-app ──(Bearer token, private network)──> beloapi :8787 ──> airbnb.com
```

Keep BeloAPI on a private network or behind TLS termination. It is a proxy, not
an application server: it has no user accounts and no per-user quotas.

---

## 8. Deployment

### Windows

Run it as a service so it survives reboots:

- **NSSM:** `nssm install BeloAPI C:\path\beloapi.exe` then add arguments
  `--allow-public --host 0.0.0.0` and the environment variable
  `BELOAPI_TOKEN`.
- **Task Scheduler:** action = `beloapi.exe`, "run whether user is logged on or
  not", trigger = at startup.

Autostart on a workstation is covered in [SETUP.md](SETUP.md#4-run-in-the-background).

### Linux (systemd)

Linux support is **build-from-source and currently unverified** — the released
binary is Windows only. If you build it there, a unit looks like this:

```ini
[Unit]
Description=BeloAPI - unofficial Airbnb proxy
After=network-online.target

[Service]
ExecStart=/usr/local/bin/beloapi --host 127.0.0.1 --port 8787
Environment=BELOAPI_TOKEN=replace-me
Restart=on-failure
# Keep the token out of the process list; use a file if you prefer
User=beloapi

[Install]
WantedBy=multi-user.target
```

Bind to `127.0.0.1` and let nginx hold the public port — that gives you TLS and
rate limiting for free, and keeps the token off the internet.

### nginx reverse proxy

```nginx
server {
    listen 443 ssl;
    server_name beloapi.example.com;

    ssl_certificate     /etc/letsencrypt/live/beloapi.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/beloapi.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8787;
        proxy_set_header Host $host;
        proxy_read_timeout 90s;      # a search can take a couple of seconds
        limit_req zone=beloapi burst=10 nodelay;
    }
}
```

Add `limit_req_zone $binary_remote_addr zone=beloapi:10m rate=5r/s;` in the
`http` block. BeloAPI has **no built-in rate limiting**: the upstream is
Airbnb's public web API, and hammering it will get you throttled.

### Health checks

Point load balancers and container probes at `GET /api/health`. It is
loopback-cheap and does not call upstream. If it returns 401, your probe is
missing the token.

---

## 9. Performance and limits

Measured locally against a live upstream (Nairobi → airbnb.com):

| Endpoint | Typical latency | Typical size |
| --- | --- | --- |
| `/api/health`, `/`, `/api/config` | < 10 ms (config ~0.4 s) | bytes |
| `/api/autocomplete` | 0.4–0.5 s | 6–8 KB |
| `/api/markets` | 0.3–0.5 s | ~440 B |
| `/api/search?items=18` | 1.0–1.7 s | 140–260 KB |
| `/api/homepage` | 0.6–1.0 s | 230–270 KB |

Server-side limits and behaviour:

| Setting | Value |
| --- | --- |
| Upstream connect/send timeout | 15 s (Windows), 90 s total via `curl` elsewhere |
| Upstream receive timeout | 60 s |
| Max request headers | 64 KB |
| Max request body | 8 MB |
| Concurrency | thread per connection; no queue limit |
| Built-in rate limiting | none — put it in your proxy |
| Built-in caching | none — add yours |

**Caching advice.** Price data changes slowly. Cache `/api/search` responses for
5–15 minutes keyed on every parameter, autocomplete for hours, and `/api/config`
for minutes. This is the difference between a responsive app and one that
depends on Airbnb's latency for every page view.

**Trim before storing or sending to a client.** A 187 KB search document is
mostly presentation metadata. Extract the dozen fields in the table above and
store those; keep the raw document only if you need to re-parse it later.

---

## 10. Stability and limits

- **Unofficial.** BeloAPI sends the same requests the airbnb.com browser sends,
  using the public web configuration served to every visitor. It is not
  affiliated with Airbnb, Inc.
- **Persisted-query hashes expire.** Airbnb rotates the hashes its web client
  uses. When that happens a route starts failing until the hash is updated —
  this is expected maintenance, tracked in
  [CONTRIBUTING.md](../CONTRIBUTING.md).
- **No writes.** No booking, no login, no account data, no reviews text, no
  listing-detail document, no availability calendar. Read-only browsing data
  only.
- **Terms.** Respect Airbnb's Terms of Service and local law. Do not use it to
  build a competing booking service or to republish inventory at scale.

When something looks wrong, run `beloapi.exe --test`: it calls all nine upstream
routes and prints a PASS/FAIL line with status and byte count, which separates
"my app is broken" from "upstream changed".

---

*BeloAPI is MIT licensed. Made by Dennis Nganga Mburu (CrackBoy), Kenya —
crackboyshop@proton.me.*
