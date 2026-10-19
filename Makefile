# Host-side build & test (macOS/Linux).  The OPENSTEP build lives in Makefile.openstep.
# Flags approximate what gcc 2.7.2 will accept: strict C89, no // comments,
# no mixed declarations, no stdint.
CC      ?= cc
CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Wdeclaration-after-statement \
          -Wno-long-long -Wno-unused-parameter -O2 -g -Icore
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

TESTS = prims crypto bignum ecc rsa der x509 tls_wire tls_prf tls_aead tls tls_pins json

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

.PHONY: all test lint clean tls-smoke

.SECONDARY:
