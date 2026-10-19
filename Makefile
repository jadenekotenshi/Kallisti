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

TESTS = prims crypto bignum ecc rsa der x509 tls_wire tls_prf tls_aead tls tls_pins json http ws gateway rest model

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

.PHONY: all test lint clean tls-smoke http-smoke ws-smoke gw-smoke

.SECONDARY:
