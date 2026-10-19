#!/bin/sh
# Regenerates the certificate fixtures in tests/certs/ (committed; only needed when a case is added).
# Everything is throwaway: keys are discarded, certificates are valid for ~100 years.
set -e
cd "$(dirname "$0")/certs"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
DAYS=36500

rsakey() { openssl genrsa -out "$1" 2048 2>/dev/null; }
eckey()  { openssl ecparam -name "$2" -genkey -noout -out "$1" 2>/dev/null; }

# mkca NAME KEYFILE SUBJECT EXTFILE [SIGNER_CERT SIGNER_KEY HASH]
mkcert() {
    name=$1; key=$2; subj=$3; ext=$4; signer=$5; skey=$6; hash=${7:-sha256}
    openssl req -new -key "$key" -subj "$subj" -out "$T/$name.csr" 2>/dev/null
    if [ -z "$signer" ]; then
        openssl x509 -req -in "$T/$name.csr" -signkey "$key" -days $DAYS -$hash -extfile "$ext" -out "$name.pem" 2>/dev/null
    else
        openssl x509 -req -in "$T/$name.csr" -CA "$signer" -CAkey "$skey" -set_serial "$(date +%s)$RANDOM" -days $DAYS -$hash \
            -extfile "$ext" -out "$name.pem" 2>/dev/null
    fi
}

cat > "$T/ca.ext" <<X
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
X
cat > "$T/ca0.ext" <<X
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
X
cat > "$T/ca1.ext" <<X
basicConstraints=critical,CA:TRUE,pathlen:1
keyUsage=critical,keyCertSign,cRLSign
X
cat > "$T/notca.ext" <<X
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
X
cat > "$T/noku.ext" <<X
basicConstraints=critical,CA:TRUE
X
cat > "$T/leaf.ext" <<X
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:example.test,DNS:*.wild.test,DNS:*.com,IP:10.1.2.3
X
cat > "$T/leaf_client.ext" <<X
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
subjectAltName=DNS:example.test
X
cat > "$T/leaf_keyenc.ext" <<X
basicConstraints=CA:FALSE
keyUsage=critical,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:example.test
X
cat > "$T/leaf_nosan.ext" <<X
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
X
cat > "$T/leaf_crit.ext" <<X
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:example.test
1.2.3.4.5=critical,ASN1:UTF8String:hello
X

rsakey "$T/rootA.key"; eckey "$T/interA.key" prime256v1; eckey "$T/leafA.key" prime256v1
mkcert rootA "$T/rootA.key" "/CN=Test Root A" "$T/ca.ext"
mkcert interA "$T/interA.key" "/CN=Test Intermediate A" "$T/ca0.ext" rootA.pem "$T/rootA.key" sha384
mkcert leafA "$T/leafA.key" "/CN=example.test" "$T/leaf.ext" interA.pem "$T/interA.key" sha256

# chain B: an EC P-384 root signing an RSA leaf directly
eckey "$T/rootB.key" secp384r1; rsakey "$T/leafB.key"
mkcert rootB "$T/rootB.key" "/CN=Test Root B" "$T/ca.ext"
mkcert leafB "$T/leafB.key" "/CN=example.test" "$T/leaf.ext" rootB.pem "$T/rootB.key" sha384

# a different CA that claims the same name as root A
rsakey "$T/rogue.key"
mkcert rogueA "$T/rogue.key" "/CN=Test Root A" "$T/ca.ext"

# an intermediate that is not a CA, and one with no basicConstraints
eckey "$T/notca.key" prime256v1; eckey "$T/l1.key" prime256v1
mkcert notca "$T/notca.key" "/CN=Not A CA" "$T/notca.ext" rootA.pem "$T/rootA.key"
mkcert leaf_via_notca "$T/l1.key" "/CN=example.test" "$T/leaf.ext" notca.pem "$T/notca.key"
cat > "$T/nobc.ext" <<X
keyUsage=critical,keyCertSign
X
eckey "$T/nobc.key" prime256v1; eckey "$T/l1b.key" prime256v1
mkcert nobc "$T/nobc.key" "/CN=No BasicConstraints" "$T/nobc.ext" rootA.pem "$T/rootA.key"
mkcert leaf_via_nobc "$T/l1b.key" "/CN=example.test" "$T/leaf.ext" nobc.pem "$T/nobc.key"

# path length: root -> p0 (pathlen 0) -> q (CA) -> leaf must fail; root -> p1 (pathlen 1) -> q -> leaf must pass
for v in 0 1; do
  eckey "$T/p$v.key" prime256v1; eckey "$T/q$v.key" prime256v1; eckey "$T/lp$v.key" prime256v1
  mkcert p$v "$T/p$v.key" "/CN=Path P$v" "$T/ca$v.ext" rootA.pem "$T/rootA.key"
  mkcert q$v "$T/q$v.key" "/CN=Path Q$v" "$T/ca.ext" p$v.pem "$T/p$v.key"
  mkcert leaf_path$v "$T/lp$v.key" "/CN=example.test" "$T/leaf.ext" q$v.pem "$T/q$v.key"
done

# leaf variants signed directly by interA's key
for c in client keyenc nosan crit; do
  eckey "$T/l_$c.key" prime256v1
  mkcert leaf_$c "$T/l_$c.key" "/CN=example.test" "$T/leaf_$c.ext" interA.pem "$T/interA.key"
done
# a SHA-1 signed leaf (RSA signing key)
rsakey "$T/sha1root.key"; eckey "$T/l_sha1.key" prime256v1
mkcert sha1root "$T/sha1root.key" "/CN=Test Root SHA1" "$T/ca.ext"
mkcert leaf_sha1 "$T/l_sha1.key" "/CN=example.test" "$T/leaf.ext" sha1root.pem "$T/sha1root.key" sha1
# an RSA-1024 leaf
openssl genrsa -out "$T/weak.key" 1024 2>/dev/null
mkcert leaf_weak "$T/weak.key" "/CN=example.test" "$T/leaf.ext" rootA.pem "$T/rootA.key"
# a self-signed leaf
eckey "$T/self.key" prime256v1
mkcert selfsigned "$T/self.key" "/CN=example.test" "$T/leaf.ext"
# rootA reissued with a new key (same name), for the "updated root" case
rsakey "$T/rootA2.key"; mkcert rootA2 "$T/rootA2.key" "/CN=Test Root A" "$T/ca.ext"
eckey "$T/leafA2.key" prime256v1
mkcert leafA2 "$T/leafA2.key" "/CN=example.test" "$T/leaf.ext" rootA2.pem "$T/rootA2.key"
ls
