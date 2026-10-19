#!/bin/sh
# Builds a throwaway CA -> intermediate -> leaf (CN/SAN localhost, RSA) in build/trustpki/ for trust_smoke.
set -e
D=build/trustpki
rm -rf $D; mkdir -p $D
cat > $D/ca.ext <<X
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign
X
cat > $D/leaf.ext <<X
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost
X
for n in ca inter leaf other; do openssl genrsa -out $D/$n.key 2048 2>/dev/null; done
openssl req -new -key $D/ca.key -subj "/CN=Smoke CA" -out $D/ca.csr 2>/dev/null
openssl x509 -req -in $D/ca.csr -signkey $D/ca.key -days 30 -sha256 -extfile $D/ca.ext -out $D/ca.pem 2>/dev/null
openssl req -new -key $D/other.key -subj "/CN=Other CA" -out $D/other.csr 2>/dev/null
openssl x509 -req -in $D/other.csr -signkey $D/other.key -days 30 -sha256 -extfile $D/ca.ext -out $D/other.pem 2>/dev/null
openssl req -new -key $D/inter.key -subj "/CN=Smoke Intermediate" -out $D/inter.csr 2>/dev/null
openssl x509 -req -in $D/inter.csr -CA $D/ca.pem -CAkey $D/ca.key -set_serial 2 -days 30 -sha256 -extfile $D/ca.ext -out $D/inter.pem 2>/dev/null
openssl req -new -key $D/leaf.key -subj "/CN=localhost" -out $D/leaf.csr 2>/dev/null
openssl x509 -req -in $D/leaf.csr -CA $D/inter.pem -CAkey $D/inter.key -set_serial 3 -days 30 -sha256 -extfile $D/leaf.ext -out $D/leaf.pem 2>/dev/null
cat $D/leaf.pem $D/inter.pem > $D/chain.pem
