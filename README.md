# Kallisti

A native Discord client for OPENSTEP 4.2, in C89/Objective-C. Fifth in the family with
[StepSSH](https://github.com/jadenekotenshi/StepSSH), [StepTTY](https://github.com/jadenekotenshi/StepTTY),
[RatChat](https://github.com/jadenekotenshi/RatChat) and StepAmp: it vendors RatChat's crypto and TLS 1.2
core (copy-and-diverge, not a shared library) and reuses its hardware-proven AppKit patterns (hand-built
windows, a polled run-loop timer, the entropy-seeding panel). One window shows every server and channel.

**Status: v0.1.0.** Runs on real OPENSTEP 4.2 (i386): signing in, servers and channels, history and live
messages, sending, direct messages, inline pictures and GIF animation, custom and Unicode emoji, reactions, file
downloads and uploads (including drag and drop), typing indicators, proxies and saving a channel's log have all
been used there. The m68k and SPARC builds, the fat binary and the installer package have not been tried yet.
`make test` and the smoke tests (`ui-smoke`, `client-smoke`, ...) run on the development host against a fake
Discord, and the engine has been run live against discord.com. What exists:

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

6. ~~**Client engine** (`core/client.c`, `core/netconn.c`)~~ -- done: `netconn` is one polled non-blocking TCP
   connection with the TLS engine and certificate validation on top; `client` runs the gateway session and the
   REST queue over two such connections (reconnect and resume by itself, an idle keep-alive connection that
   the server dropped is re-opened once transparently), checks the token with `GET /users/@me` before the
   gateway so a bad token is named at once, and feeds the model. `kc_tick()` from a timer is all an app does.
7. ~~**The app** (`app/`)~~ -- written: a sign-in panel (the token in a bullet-only field with paste, "remember"
   and a clearly labelled user-token checkbox), then one window: servers and channels on the left (click a
   server to fold it; unread counts, `!` when you were mentioned), messages on the right with local-time stamps,
   day separators and coloured names, a field to type into, and a status line. Closing the window quits.
   `core/kprefs.c` keeps `~/.kallisti/prefs` (mode 600; the token only if you ticked "remember").

8. **Channel windows, DMs and pictures** (`app/ChannelPane`, `ChannelWindow`, `DMController`, `ImageWindow`,
   `ImageListController`; `core/png.c`, `inflate.c`, `img.c`):
   - Double-click a channel in the list (or *Discord > Open Channel in New Window*) for a window of its own.
   - Direct messages appear under a *Direct Messages* entry at the top of the list, as they arrive (a message
     in an unknown DM creates the conversation) or, for a user-account token, from its READY. They always count as
     mentions. *Discord > New Direct Message...* opens one by user ID. A bot can only DM people who share a server
     with it and cannot list its DMs.
   - Pictures: messages show `[image: cat.png 800x600]`; *Discord > Images in This Channel...* lists them and
     opens one in its own window. Kallisti asks Discord's media proxy for a resized picture (a JPEG when the
     original is a JPEG -- about a tenth the bytes of the PNG -- otherwise a PNG; if the proxy will not make a JPEG
     it asks again for a PNG), decodes it itself and gives AppKit an uncompressed TIFF, so AppKit needs no decoder
     and nothing is linked in. `core/png.c` reads every PNG type with interlacing and transparency. `core/jpeg.c`
     reads baseline, extended and progressive Huffman JPEGs (any subsampling, restart intervals, grey) and uses
     libjpeg's published integer IDCT, upsampling and colour arithmetic, so its output is bit-for-bit libjpeg's:
     the 44 test pictures in `tests/jpg` (made by libjpeg through Pillow) must match libjpeg's own decode exactly.
     `core/gif.c` reads GIF87a/89a (palettes, interlacing, transparency, any LZW stream) and shows an animation's
     first frame; whatever the proxy sends back is recognised by its first bytes, so a GIF where a PNG was asked for
     is not a problem. Another site's picture (a link preview from outside Discord) is fetched exactly as addressed.
   - Embeds and stickers: a link preview shows under its message as grey lines (`| Example`, `| Title`, `| first
     lines of the description`, fields, footer, and `[image: pic.jpg 100x50]`); a bare image link or a GIF from
     Tenor shows just its picture or poster frame. The preview's picture is in *Images in This Channel* and, with the
     experimental switch, inline. Previews that Discord adds a moment after the message (MESSAGE_UPDATE) appear when
     they arrive. Stickers show as `[sticker: Wave]` and their PNG/GIF is in the picture list (Lottie stickers
     cannot be shown).
   - Reactions: each message shows its reactions as a grey line of `[emoji count]` boxes (a `*` after the count marks
     one you made); Unicode and custom emoji are drawn as pictures like everywhere else. They arrive with the
     message and are kept current by the gateway's reaction events (the bot asks for the guild and DM reaction
     intents, which need no Developer Portal switch).
   - The message box grows as you type, up to about six lines, then scrolls. Return sends; Shift- or Alt-Return
     starts a new line.
   - Downloads: Discord > Attachments in This Channel... (Cmd-I) lists every file attached to the messages shown, not only
     pictures. Save... (or a double-click on a file that is not a picture) asks where, then fetches the file from
     Discord's own address, untouched (up to 32 MB), and writes it as it came. Open still shows pictures in the viewer.
   - Sending files: drag files from the Workspace onto the message log or the input, or choose Discord > Attach File...
     (Cmd-U). They go as one message (up to 10 files, 10 MB in all, plain files only) with whatever is typed in the input
     as its text; the input is emptied. The files are uploaded as `multipart/form-data` in one request, so a slow link
     shows "Sending n files..." until Discord's reply puts the message in the log. Dragging is the part to check on real
     hardware; Attach File... is the same path without it.
   - Server names are bold in the list, so they stand out from the channels under them.
   - Discord > Save Message Log... (Cmd-S) writes the messages held for the channel in front (a channel window if one is
     key, else the main window's) to a text file: a title, date separators, `[HH:MM] author: text`, embeds, reactions,
     and the address of each attachment. It is UTF-8, with emoji written as `:name:` when the emoji pack is loaded, so it
     reads sensibly even in an editor that knows no Unicode. Only what is in memory is saved (the latest 50 messages
     loaded when the channel is opened, plus what has arrived since, up to 200).
   - Reacting: Discord > React to a Message... (Cmd-R) lists the latest messages; pick one and type an emoji --
     `:thumbsup:`, a custom emoji's `:name:` (one seen in the channel), a pasted `<:name:id>`, or the emoji itself -- or
     choose from the Common menu. React adds yours, Remove Mine takes it back. The bot needs the Add Reactions
     permission; a refusal appears in the log.
   - Who is typing: a small line between the log and the input says "Ann is typing...", "Ann and Bob are typing...",
     or "Several people are typing...". Each TYPING_START counts for 10 seconds, and a message from that person ends it. Typing in the message box tells Discord too (at most every 8 seconds per
     channel), so others see you typing.
   - Unicode emoji: OPENSTEP's fonts have none, so Kallisti draws them as pictures. `app/emoji.pack` (2.5 MB, copied
     into the application bundle by `Makefile.openstep`) holds a 16x16 picture for each of the 3,838 emoji Twemoji 15.1
     has; `core/emoji.c` finds the emoji in a message -- skin tones, flags, keycaps, joined families, tag flags -- by
     Twemoji's own file-naming rules and the pictures stand in for them, with no network involved. A bare (c), (r),
     (tm), digit or plain arrow stays text (fonts have them); `FE0F` makes them emoji. Discord > *Show Emoji as
     Pictures* switches it off. Where no picture can go (the server and channel list, window titles, the status line,
     alerts, or anywhere with pictures off) an emoji is spelt as its shortcode, `:grinning:`, `:thumbsup::skin-tone-3:`,
     instead of a question mark; in the message log a name with an emoji in it gets the picture too. Typing a shortcode
     (`:smile:`, `:thumbsup:`, `:+1:`, `:flag_us:`, any of 2,170 names from GitHub's gemoji list, in any case) sends the
     emoji, as Discord's own client does -- but not inside `code` or a code block, not as part of a custom emoji tag, and
     not right after a letter or digit (`12:30:smile:`). The pack carries the names, the pictures and the licence
     (`tools/gen_emoji.py` rebuilds it from jsDelivr and gemoji; it needs Pillow). The pictures are CC-BY 4.0, see
     `EMOJI-LICENSE.txt`; the names are from gemoji (MIT).
   - Custom emoji: `<:blob:123>` shows as `:blob:`; with *Show Images Inline* the emoji's own picture (Discord's 32-pixel
     PNG, shown at 16) takes its place in the line, each fetched once. Animated emoji show their first frame.
   - Animated GIFs: opening a GIF from the picture list fetches the GIF itself (resized to fit 400x300 -- no `format`
     parameter, so it stays a GIF), decodes every frame composited onto the screen with all three disposal methods, and
     plays it in the viewer window on a timer (delays as browsers read them: 0 or 10 ms means 100 ms). At most 100 frames
     and 3 million pixels in all are kept, so the memory a GIF can take is bounded; an animation cut short in the file
     plays what is there. Inline thumbnails stay still pictures. *Show Images Inline (Experimental)* draws thumbnails inside the log
     with `NSTextAttachment`, which is untested on OPENSTEP; leave it off if the log misbehaves.

**v0.1 scope:** connect, list servers and their text channels, read live messages, send messages -- plus the
three additions above. No reactions, embeds beyond plain text, or voice. Known limits: characters outside the
Basic Multilingual Plane that are not emoji show as `?`; threads and voice channels are not listed; channels are not
filtered by the bot's permissions (opening one it cannot read says so in the message view); animated GIFs show
their first frame; user-token mode is untested (no intents are sent for it and nothing else differs). Decoding a
large picture takes seconds on a 68040; arithmetic-coded, 12-bit, lossless and CMYK JPEGs are refused.

## Proxies

Every connection (REST, the gateway WebSocket and pictures) can go through a proxy, with TLS still end to end
through the tunnel (the proxy sees only the destination's name and port; the certificate is validated as usual).
The sign-in panel has *Proxy*, *Proxy user* and *Proxy password* fields, saved in `~/.kallisti/prefs` (mode 600;
the password only if you tick "remember", like the token).

- `http://host:3128` (or just `host:3128`): an HTTP proxy asked to `CONNECT`; `socks5://host:1080` (also `socks5h://`
  and `socks://`): a SOCKS5 proxy, which resolves the server's name itself. A login can be written in the address
  (`http://bob:secret@host:3128`, with `%40`-style escapes) or in the separate fields (which win). HTTP proxies get
  Basic authentication; SOCKS5 gets user/password. SOCKS4 and proxies you reach over TLS are not supported.
- With the field empty, Kallisti falls back to the environment: `https_proxy`, `HTTPS_PROXY`, `all_proxy`,
  `ALL_PROXY`, `http_proxy`, `HTTP_PROXY` (in that order), minus any host matched by `no_proxy` / `NO_PROXY`
  (`*`, names, `.domain` suffixes). (A Workspace-launched app does not inherit a shell's exports; start it from a
  shell if you want this.)
- A bad setting is reported at sign-in; a proxy that cannot be reached, wants a login, or refuses the destination says
  so in the error. The status line reads "Connected as NAME (through the proxy host:port)".

Pictures are fetched over one connection that is kept for the next picture (and dropped if the server closes it,
says `Connection: close`, or it sits idle for 20 s), so a channel full of inline thumbnails pays for one TLS
handshake, not one each.

## Logging in: bot tokens and user tokens

- **Bot token (the supported way, built first).** Create an application and bot in the Discord Developer
  Portal, invite it to your server, and paste its token into Kallisti. The bot only sees servers it has been
  added to, cannot act as your personal account, and has no DMs with other users. To read message text the
  bot needs the **Message Content** privileged intent switched on in the portal.
- **User token (opt-in).** Logging in as a personal account from a third-party client is against Discord's
  terms of service and can get the account banned. Kallisti accepts either kind of token, but user-token mode
  is a clearly labelled checkbox that asks for confirmation, is off by default, and is experimental.

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
    make proxy-smoke   # HTTP CONNECT and SOCKS5 proxies (with and without a login) against proxies written in Python
    make client-smoke  # the client engine end to end against a scripted fake Discord, plain and TLS
    make check-objc    # syntax-check app/*.m against the modern SDK (never linked)
    make ui-smoke      # the real windows and controllers, driven like a user, against the fake Discord (macOS only)
    make lint          # OPENSTEP-compatibility checks

On OPENSTEP (NeXT make or GNU make, from this directory):

    make -f Makefile.openstep test   # the C unit tests on the real compiler -- run this first
    make -f Makefile.openstep        # Kallisti.app
    make -f Makefile.openstep fat    # i386 + m68k + SPARC fat binary (three tuned passes, as in the siblings)
    make -f Makefile.openstep pkg    # Kallisti.pkg for Installer.app (dist / pkg-fat / dist-fat too)

`make dist` on the Mac packs the sources into `dist/KALLISTI.TAR` and an ISO to attach to the VM. If the app does
nothing when launched from Workspace, `touch ~/.Kallisti.trace` and launch again: it narrates startup there.
The first connection needs random numbers: with no `/dev/urandom` the app asks you to move the mouse until a
bar fills (the seed is then kept in `~/.kallisti/rng_seed`). Put updated CA certificates in `/usr/local/ssl`
to override the compiled-in roots, and use *Discord > Ignore Certificate Dates* only if the machine's clock is wrong.
