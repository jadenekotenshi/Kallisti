# Host-side build & test (macOS/Linux).  The OPENSTEP build lives in Makefile.openstep.
# Flags approximate what gcc 2.7.2 will accept: strict C89, no // comments,
# no mixed declarations, no stdint.
CC      ?= cc
CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Wdeclaration-after-statement \
          -Wno-long-long -Wno-unused-parameter -Wno-deprecated-declarations -O2 -g -Icore
BUILD   = build
SANFLAGS =
# make SAN=1 test   -- build everything with AddressSanitizer + UBSan in build-san/
ifdef SAN
SANFLAGS = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS  += $(SANFLAGS)
BUILD   = build-san
endif

CORE_SRC = $(wildcard core/*.c)
CORE_OBJ = $(patsubst core/%.c,$(BUILD)/core_%.o,$(CORE_SRC))

TESTS = prims crypto bignum ecc rsa der x509 tls_wire tls_prf tls_aead tls tls_pins json http ws gateway rest model chain roots kprefs ktime png

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/core_%.o: core/%.c $(wildcard core/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_%: tests/test_%.c $(wildcard tests/*.h) $(CORE_OBJ)
	$(CC) $(CFLAGS) tests/test_$*.c $(CORE_OBJ) -o $@

test: $(addprefix $(BUILD)/test_,$(TESTS))
	@for t in $(TESTS); do $(BUILD)/test_$$t || exit 1; done

lint:
	sh tools/lint_openstep.sh

clean:
	rm -rf build build-san

# Drives a real tls_session through a full TLS 1.2 handshake against a REAL local `openssl s_server`
# (a throwaway self-signed cert generated fresh each run).
tls-smoke:
	mkdir -p build
	openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 -subj "/CN=localhost" \
	    -keyout build/tls_smoke.key -out build/tls_smoke.pem 2>/dev/null
	$(CC) -w -g -Icore tests/tls_smoke.c core/*.c -o build/tls_smoke
	build/tls_smoke

# Drives core/http.c against a scripted real HTTP server (tests/http_smoke_server.py), in plaintext and
# through the vendored TLS engine (the server wraps itself in TLS with a throwaway certificate).
http-smoke:
	mkdir -p build
	openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 -subj "/CN=localhost" \
	    -keyout build/http_smoke.key -out build/http_smoke.pem 2>/dev/null
	$(CC) -w -g $(SANFLAGS) -Icore tests/http_smoke.c core/*.c -o build/http_smoke
	build/http_smoke tests/http_smoke_server.py 15911 15912 build/http_smoke.pem build/http_smoke.key

# Drives core/ws.c (with core/http.c for the upgrade) against Python's `websockets` library, an independent
# implementation, in plaintext and over TLS.  The first run creates build/venv and installs `websockets`
# into it (needs network once).
build/venv/bin/python:
	mkdir -p build
	python3 -m venv build/venv
	build/venv/bin/pip install -q websockets

ws-smoke: build/venv/bin/python
	openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 -subj "/CN=localhost" \
	    -keyout build/ws_smoke.key -out build/ws_smoke.pem 2>/dev/null
	$(CC) -w -g $(SANFLAGS) -Icore tests/ws_smoke.c core/*.c -o build/ws_smoke
	build/ws_smoke tests/ws_smoke_server.py build/venv/bin/python 15913 15914 build/ws_smoke.pem build/ws_smoke.key

# Drives core/gateway.c end to end against a scripted fake gateway (same library as ws-smoke), plain and TLS.
gw-smoke: build/venv/bin/python
	openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 -subj "/CN=localhost" \
	    -keyout build/gw_smoke.key -out build/gw_smoke.pem 2>/dev/null
	$(CC) -w -g $(SANFLAGS) -Icore tests/gw_smoke.c core/*.c -o build/gw_smoke
	build/gw_smoke tests/gw_smoke_server.py build/venv/bin/python 15915 15916 build/gw_smoke.pem build/gw_smoke.key build/gw_smoke.report

# TLS certificate validation against a real TLS server and a throwaway CA -> intermediate -> leaf chain.
trust-smoke:
	tests/trust_smoke_setup.sh
	$(CC) -w -g $(SANFLAGS) -Icore tests/trust_smoke.c core/*.c -o build/trust_smoke
	build/trust_smoke tests/http_smoke_server.py 15917 build/trustpki/chain.pem build/trustpki/leaf.key \
	    build/trustpki/ca.pem build/trustpki/other.pem

# The client engine end to end against a scripted fake Discord, plain and TLS (validated against a test CA).
client-smoke: build/venv/bin/python
	tests/trust_smoke_setup.sh
	$(CC) -w -g $(SANFLAGS) -Icore tests/client_smoke.c core/*.c -o build/client_smoke
	build/client_smoke tests/client_smoke_server.py build/venv/bin/python 15920 15930 build/trustpki/chain.pem \
	    build/trustpki/leaf.key build/trustpki/ca.pem

# The whole stack against the real Discord: TLS validation with the compiled-in roots, REST, WebSocket (needs
# network; no token needed -- it only reads the public /gateway endpoint and the gateway's HELLO).
discord-smoke:
	$(CC) -w -g $(SANFLAGS) -Icore tests/discord_smoke.c core/*.c -o build/discord_smoke
	build/discord_smoke

# Package the sources for transfer into the OPENSTEP VM.
#   dist/KALLISTI.TAR  plain ustar archive (extract with:  tar xf KALLISTI.TAR)
#   dist/KALLISTI.ISO  a CD image containing KALLISTI.TAR (attach it as a CD-ROM in the VM)
# -b 20 matters: old tar implementations read archives in fixed 10240-byte records, and a short final record
# makes their first read() look like premature EOF.
DISTFILES = README.md Makefile.openstep core app tests tools
dist:
	mkdir -p dist
	COPYFILE_DISABLE=1 tar --format ustar -b 20 --exclude '*.o' --exclude '.DS_Store' \
	    -cf dist/KALLISTI.TAR $(DISTFILES)
	rm -rf dist/iso && mkdir -p dist/iso && cp dist/KALLISTI.TAR dist/iso/
	rm -f dist/KALLISTI.ISO dist/KALLISTI.iso dist/KALLISTI.iso.iso
	hdiutil makehybrid -iso -iso-volume-name KALLISTI -o dist/KALLISTI dist/iso >/dev/null
	f=$$(ls dist/KALLISTI.* | grep -iv 'KALLISTI.TAR' | head -1); mv "$$f" dist/KALLISTI.ISO
	rm -rf dist/iso
	@ls -l dist/KALLISTI.TAR dist/KALLISTI.ISO

# Syntax-check the Objective-C against the modern SDK (never linked or run).
check-objc:
	for f in app/*.m; do \
	  echo "check $$f"; \
	  $(CC) -fsyntax-only -x objective-c -fno-objc-arc -Wall -Wno-deprecated-declarations \
	    -Wdeclaration-after-statement -Wno-unused-parameter -Iapp -Icore $$f || exit 1; \
	done

# The real AppController/MainController stack, with the modern Cocoa on this Mac, against the fake Discord.
UI_SRC = app/AppController.m app/LoginController.m app/MainController.m app/ChannelPane.m app/ChannelWindow.m \
         app/DMController.m app/ImageWindow.m app/ImageListController.m app/SecretField.m app/UIHelpers.m
ui-smoke: build/venv/bin/python
	mkdir -p build/ui
	for f in tests/ui_smoke.m $(UI_SRC); do \
	  $(CC) -c -x objective-c -fno-objc-arc -w -g -Iapp -Icore $$f -o build/ui/$$(basename $$f .m).o || exit 1; \
	done
	for f in core/*.c; do $(CC) -c -w -g -Icore $$f -o build/ui/c_$$(basename $$f .c).o || exit 1; done
	$(CC) build/ui/*.o -framework Cocoa -o build/ui_smoke
	build/ui_smoke tests/client_smoke_server.py build/venv/bin/python 15940

.PHONY: all test lint clean tls-smoke http-smoke ws-smoke gw-smoke trust-smoke discord-smoke client-smoke check-objc ui-smoke dist

.SECONDARY:
