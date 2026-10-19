/*
 * chain.h -- X.509 certificate chain validation for the TLS client: a trust store of root certificates,
 * and chain_verify(), which checks that the certificates a server presented lead from its leaf to one of
 * those roots, and that the leaf is valid for the host name we meant to reach.
 *
 * What is checked: every signature along the path (RSA PKCS#1 v1.5 and ECDSA, SHA-256/384/512; SHA-1 and
 * MD5 signatures are refused), validity dates (optionally: see `now`), that every issuer is a CA
 * (basicConstraints cA, keyCertSign if keyUsage is present, pathLenConstraint), the leaf's extended key
 * usage (serverAuth) and key usage (digitalSignature), the host name against the subjectAltName DNS names
 * (and IPv4 addresses), and that no critical extension we do not understand is present.  The chain is built
 * from the presented certificates in any order, so a server that sends an extra or a cross-signed
 * certificate still validates.  Not checked: revocation, name constraints (a certificate that marks them
 * critical is refused), policies.
 */
#ifndef KALLISTI_CHAIN_H
#define KALLISTI_CHAIN_H

#include "ssh_types.h"
#include "der.h"
#include "x509.h"

#define CHAIN_MAX_CERTS 10
#define CHAIN_MAX_DEPTH 8
#define XCERT_MAX_SAN 48

enum { XSIG_NONE = 0, XSIG_RSA_SHA256, XSIG_RSA_SHA384, XSIG_RSA_SHA512,
       XSIG_ECDSA_SHA256, XSIG_ECDSA_SHA384, XSIG_ECDSA_SHA512 };

/* A parsed certificate.  Pointers refer into the DER buffer, which must outlive it. */
typedef struct {
    const u8 *der;
    size_t der_len;
    const u8 *tbs;               /* the TBSCertificate with its header: what the signature covers */
    size_t tbs_len;
    const u8 *issuer;            /* the Name, header included, for byte-wise comparison */
    size_t issuer_len;
    const u8 *subject;
    size_t subject_len;
    int sig_alg;                 /* XSIG_*, or XSIG_NONE if unsupported (the reason is in sig_unsupported) */
    int sig_unsupported;
    const u8 *sig;               /* signature value bytes */
    size_t sig_len;
    x509_cert key;               /* the subject public key and validity */
    int version;
    int has_bc, is_ca, has_pathlen;
    long pathlen;
    int has_ku;
    unsigned ku;                 /* first keyUsage byte: 0x80 digitalSignature, 0x04 keyCertSign */
    int has_eku, eku_server;
    int unknown_critical;
    struct { const u8 *p; size_t n; } dns[XCERT_MAX_SAN];
    int ndns;
    struct { u8 b[4]; } ip4[8];
    int nip4;
} XCert;

void xcert_init(XCert *c);
void xcert_free(XCert *c);
/* Parses a whole certificate (the buffer must be exactly one DER certificate).  0 or -1 with *err. */
int xcert_parse(XCert *c, const u8 *der, size_t len, const char **err);
/* 1 if the certificate is valid for this host name (or dotted IPv4 address). */
int xcert_matches_host(const XCert *c, const char *host);

/* ---- trust store ---- */

typedef struct {
    u8 *der;
    size_t der_len;
    XCert cert;
} TrustAnchor;

typedef struct {
    TrustAnchor *a;
    int n, cap;
} TrustStore;

void trust_init(TrustStore *ts);
void trust_free(TrustStore *ts);
/* Adds a root certificate (the bytes are copied).  1 if added, 0 if it was already there, -1 if it could not
 * be parsed (or is out of memory). */
int trust_add_der(TrustStore *ts, const u8 *der, size_t len);
/* Adds every "-----BEGIN CERTIFICATE-----" block in the text (other content, such as the descriptive text
 * OpenSSL's bundles carry, is skipped; blocks that do not parse are skipped).  Returns how many were added. */
int trust_add_pem(TrustStore *ts, const char *text, size_t len);

/* ---- verification ---- */

typedef struct {
    const u8 *der;
    size_t len;
} ChainCert;

/* Verifies certs[0] (the leaf) and the rest of the presented list.  `host` is the name the user asked for;
 * `now` is the current UTC time, or NULL to skip the date checks (for machines whose clock is wrong).
 * Returns 0 if the chain is trusted, else -1 with a message for the user in err. */
int chain_verify(const TrustStore *ts, const ChainCert *certs, int n, const char *host, const der_time *now,
                 char *err, size_t errcap);

#endif
