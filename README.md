# Kallisti

A native Discord client for OPENSTEP 4.2, in C89/Objective-C. Fifth in the family with
[StepSSH](https://github.com/jadenekotenshi/StepSSH), [StepTTY](https://github.com/jadenekotenshi/StepTTY),
[RatChat](https://github.com/jadenekotenshi/RatChat) and StepAmp: it vendors RatChat's crypto and TLS 1.2
core (copy-and-diverge, not a shared library) and will reuse RatChat's UI patterns (one window per channel,
a scrolling message log, a single-line input field).

**Status: scaffolding.** Nothing connects to Discord yet. What exists so far:

- `core/` -- the crypto/TLS stack from RatChat (AES-GCM, ChaCha20-Poly1305, SHA-1/2, X25519, P-256, RSA,
  X.509, TLS 1.2 client), unchanged apart from dropping the IRC and DCC parts.
- `core/json.c` -- a small JSON reader/writer (arena-allocated tree, `\uXXXX` and surrogate pairs to UTF-8,
  depth-limited; no 64-bit integers needed because Discord snowflake ids are JSON strings).
- `core/http.c` -- a sans-I/O HTTP/1.1 client, same shape as the TLS engine: a request builder (refuses CR/LF
  and other control characters in methods, paths, hosts and headers; always asks for `identity` encoding), an
  incremental response parser (Content-Length, chunked with trailers, read-until-close, 1xx skipping,
  HEAD/204/304, and a 101 that stops at the end of the headers so the WebSocket bytes stay untouched), and a
  URL parser for http/https/ws/wss.
- `core/ws.c` -- a sans-I/O WebSocket client (RFC 6455) on top of `http.c`: the upgrade handshake (nonce,
  `Sec-WebSocket-Accept` check, no extensions or subprotocols), an incremental frame decoder that reassembles
  fragments, answers pings by itself, validates UTF-8 and close codes, enforces a message size limit and turns
  every protocol violation into one error event plus a close frame, and a masked-frame encoder with an output
  queue. `core/base64.c` is its small companion.
- Host-side tests for all of it (`make test`, also clean under `make SAN=1 test`). `make http-smoke` drives the
  HTTP code against a scripted real server, in plaintext and through the vendored TLS engine.

## Plan

Discord needs three things that do not exist in the sibling projects, each as portable C89 with host tests
before any OPENSTEP code:

1. ~~**HTTP/1.1 client** (`core/http.c`)~~ -- done, see above. The REST layer on top of it (fetch the gateway
   URL, list guilds/channels, post messages, rate-limit handling) comes with the gateway work.
2. ~~**WebSocket client** (`core/ws.c`)~~ -- done, see above (for the gateway at
   `wss://gateway.discord.gg/?v=10&encoding=json`, uncompressed).
3. ~~**Gateway session** (`core/gateway.c`)~~ -- done: a sans-I/O state machine (HELLO, jittered heartbeats,
   a missed ACK drops the connection, IDENTIFY/RESUME, op 7/9, backoff, close-code policy where 4004/4010-4014
   are fatal) that hands the app READY/RESUMED/DISPATCH/DISCONNECTED/FATAL events. Drivers must drain
   `gw_next_event()` after every `gw_handle_ws_event()`, since a DISPATCH's `d` lives only until the next frame.
4. ~~**REST layer** (`core/rest.c`)~~ -- done: a sans-I/O request queue (one request in flight, FIFO) for
   `GET /users/@me`, `/gateway/bot`, `/users/@me/guilds`, `/guilds/{id}/channels`, `/channels/{id}/messages` and
   `POST /channels/{id}/messages`. Ids are validated as digit strings, message content as UTF-8 of 1-2000
   characters. It remembers `X-RateLimit-*` per route and waits instead of earning a 429; a 429 is waited out
   (up to four times, and not at all past a minute, which is reported); GETs are retried on 5xx and transport
   failures, POSTs never (the message may have been delivered). Bot tokens go out as `Bot <token>`, user tokens bare.
5. ~~**Model** (`core/model.c`)~~ -- done: servers, text/announcement channels and categories (display-ordered as
   Discord shows them), and the last 200 messages per channel, fed by gateway dispatches and REST results, with
   a change queue for the UI, unread/mention counters, mentions/emoji/attachments rendered to plain text, and
   merging that survives a fresh READY. Threads, voice, DMs and permission filtering are out of v0.1.

Then the app: connection sheet (token), a server/channel list, one window per text channel with the message
log and an input field, in the style of RatChat.

**v0.1 scope:** connect, list servers and their text channels, read live messages, send messages. No DMs,
attachments, reactions, embeds beyond plain text, or voice.

## Logging in: bot tokens and user tokens

- **Bot token (the supported way, built first).** Create an application and bot in the Discord Developer
  Portal, invite it to your server, and paste its token into Kallisti. The bot only sees servers it has been
  added to, cannot act as your personal account, and has no DMs with other users. To read message text the
  bot needs the **Message Content** privileged intent switched on in the portal.
- **User token (opt-in, later).** Logging in as a personal account from a third-party client is against
  Discord's terms of service and can get the account banned. Kallisti is being designed so a token can be
  either kind, but user-token mode will be a clearly labelled opt-in and is not the default.

Tokens are secrets: Kallisti will never log them, and tests read them from a local file or environment
variable, never from the repository.

## TLS

The vendored TLS client speaks TLS 1.2 only (ECDHE with AES-128-GCM or ChaCha20-Poly1305, RSA or ECDSA
certificates), which Discord's endpoints accept. RatChat trusts a server by pinning its leaf certificate on
first use, which suits an IRC server but not Discord's edge, whose certificates rotate often, so Kallisti
validates the certificate chain properly (`tls_set_trust()`, `core/chain.c`):

- every signature on the path (RSA and ECDSA with SHA-256/384/512; SHA-1 and MD5 are refused), validity dates,
  that each issuer is a CA with room under its `pathLenConstraint`, the leaf's key and extended key usage, the
  host name against the certificate's subjectAltName (wildcards cover exactly one label), and no unknown
  critical extensions. The chain is built from whatever the server sends in any order, so cross-signed
  intermediates (Discord's chain ends in GTS Root R4 cross-signed by GlobalSign) work. Revocation and name
  constraints are not checked.
- **Trust anchors:** `/usr/local/ssl` is read first -- `cert.pem`, `certs.pem`, `ca-bundle.crt`,
  `ca-certificates.crt`, `curl-ca-bundle.crt` there and in `certs/`, plus every `.pem`/`.crt`/`.cer` file
  and OpenSSL-hashed name (`xxxxxxxx.N`) in `certs/`. If that yields any certificates they are the trust store
  (so you can update, or remove, roots by putting files there); if not, the 28 roots compiled in from
  `core/roots_bundled.c` are used (Google Trust Services, GlobalSign, Let's Encrypt/ISRG, DigiCert, Sectigo,
  SSL.com, Amazon, Starfield). `tools/gen_roots.py` regenerates that file from any PEM bundle.
- **The clock matters.** Old machines often have a wrong date, which makes every certificate look expired or
  not yet valid; the error says so, and the date checks can be switched off by passing no time.

## Building

Host side (macOS/Linux), strict C89 flags approximating gcc 2.7.2:

    make test          # all suites
    make http-smoke    # the HTTP client against a real scripted server (needs python3 and openssl)
    make ws-smoke      # the WebSocket client against Python's `websockets` library (first run installs it into build/venv)
    make trust-smoke   # certificate validation against a real TLS server and a throwaway CA chain
    make discord-smoke # the whole stack against the real discord.com / gateway.discord.gg (needs network, no token)
    make gw-smoke      # the gateway end to end against a scripted fake gateway, plain and TLS (same venv)
    make SAN=1 test    # the same under AddressSanitizer + UBSan
    make lint          # OPENSTEP-compatibility checks (the app/ part needs Makefile.openstep, not yet here)

The OPENSTEP build (`Makefile.openstep`, the app, and a fat build for i386/m68k/SPARC) comes with the app.
