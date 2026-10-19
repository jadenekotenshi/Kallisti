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
- Host-side tests for all of it (`make test`, also clean under `make SAN=1 test`).

## Plan

Discord needs three things that do not exist in the sibling projects, each as portable C89 with host tests
before any OPENSTEP code:

1. **HTTP/1.1 client** (`core/http.c`): request building and response parsing (status, headers,
   `Content-Length` and chunked bodies) over the existing TLS session, for the REST API
   (`https://discord.com/api/v10/...`: fetch the gateway URL, list guilds/channels, post messages).
2. **WebSocket client** (`core/ws.c`): the RFC 6455 handshake (SHA-1 accept key) and framing
   (masked client frames, fragmentation, ping/pong/close) for the gateway
   (`wss://gateway.discord.gg/?v=10&encoding=json`, no compression).
3. **Gateway session** (`core/gateway.c`): HELLO, heartbeat timer, IDENTIFY, READY,
   `GUILD_CREATE` / `CHANNEL_CREATE`, `MESSAGE_CREATE`, resume on reconnect (op 6/9), rate-limit headers
   and 429 handling on REST.

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
certificates), which Discord's endpoints accept. Open question: RatChat trusts a server by pinning its leaf
certificate on first use, which suits an IRC server but not Discord's edge, whose certificates rotate often.
Kallisti will need either a bundled set of root certificates with chain validation or pinning at the
intermediate/root level.

## Building

Host side (macOS/Linux), strict C89 flags approximating gcc 2.7.2:

    make test          # all suites
    make SAN=1 test    # the same under AddressSanitizer + UBSan
    make lint          # OPENSTEP-compatibility checks (the app/ part needs Makefile.openstep, not yet here)

The OPENSTEP build (`Makefile.openstep`, the app, and a fat build for i386/m68k/SPARC) comes with the app.
