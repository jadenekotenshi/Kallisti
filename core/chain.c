/* X.509 chain validation and the trust store: see chain.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chain.h"
#include "base64.h"
#include "rsa.h"
#include "ecc.h"
#include "sha2.h"

static const u8 OID_BC[]    = { 0x55, 0x1d, 0x13 };
static const u8 OID_KU[]    = { 0x55, 0x1d, 0x0f };
static const u8 OID_EKU[]   = { 0x55, 0x1d, 0x25 };
static const u8 OID_SAN[]   = { 0x55, 0x1d, 0x11 };
static const u8 OID_POL[]   = { 0x55, 0x1d, 0x20 };
static const u8 OID_SKI[]   = { 0x55, 0x1d, 0x0e };
static const u8 OID_AKI[]   = { 0x55, 0x1d, 0x23 };
static const u8 OID_CRLDP[] = { 0x55, 0x1d, 0x1f };
static const u8 OID_SERVER_AUTH[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
static const u8 OID_ANY_EKU[]     = { 0x55, 0x1d, 0x25, 0x00 };

static const u8 OID_SHA256_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const u8 OID_SHA384_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const u8 OID_SHA512_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const u8 OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const u8 OID_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const u8 OID_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };

#define OIDEQ(o, ol, lit) der_oid_eq((o), (ol), (lit), sizeof(lit))

/* ---------------------------------------------------------------- parsing */

void xcert_init(XCert *c)
{
    memset(c, 0, sizeof(*c));
    x509_cert_init(&c->key);
}

void xcert_free(XCert *c)
{
    x509_cert_free(&c->key);
    memset(c, 0, sizeof(*c));
}

static int sig_alg_from_oid(const u8 *oid, size_t l)
{
    if (OIDEQ(oid, l, OID_SHA256_RSA)) return XSIG_RSA_SHA256;
    if (OIDEQ(oid, l, OID_SHA384_RSA)) return XSIG_RSA_SHA384;
    if (OIDEQ(oid, l, OID_SHA512_RSA)) return XSIG_RSA_SHA512;
    if (OIDEQ(oid, l, OID_ECDSA_SHA256)) return XSIG_ECDSA_SHA256;
    if (OIDEQ(oid, l, OID_ECDSA_SHA384)) return XSIG_ECDSA_SHA384;
    if (OIDEQ(oid, l, OID_ECDSA_SHA512)) return XSIG_ECDSA_SHA512;
    return XSIG_NONE;
}

static int parse_extension(XCert *c, const u8 *oid, size_t ol, int critical, const u8 *val, size_t vl)
{
    const u8 *p = val, *end = val + vl, *in, *in_end, *v;
    size_t l;
    int tag, known = 1;

    if (OIDEQ(oid, ol, OID_BC)) {
        c->has_bc = 1;
        if (der_enter(&p, end, 0x30, &in, &in_end) != 0 || p != end) return -1;
        if (in < in_end && *in == 0x01) {
            if (der_expect(&in, in_end, 0x01, &v, &l) != 0 || l != 1) return -1;
            c->is_ca = v[0] != 0;
        }
        if (in < in_end && *in == 0x02) {
            size_t i;
            long pl = 0;

            if (der_int(&in, in_end, &v, &l) != 0 || l > 2) return -1;
            for (i = 0; i < l; i++) pl = pl * 256 + v[i];
            c->has_pathlen = 1;
            c->pathlen = pl;
        }
        if (in != in_end) return -1;
    } else if (OIDEQ(oid, ol, OID_KU)) {
        int unused;

        c->has_ku = 1;
        if (der_bitstring(&p, end, &v, &l, &unused) != 0 || p != end) return -1;
        c->ku = l > 0 ? v[0] : 0;
    } else if (OIDEQ(oid, ol, OID_EKU)) {
        c->has_eku = 1;
        if (der_enter(&p, end, 0x30, &in, &in_end) != 0 || p != end) return -1;
        while (in < in_end) {
            if (der_expect(&in, in_end, 0x06, &v, &l) != 0) return -1;
            if (OIDEQ(v, l, OID_SERVER_AUTH) || OIDEQ(v, l, OID_ANY_EKU)) c->eku_server = 1;
        }
    } else if (OIDEQ(oid, ol, OID_SAN)) {
        if (der_enter(&p, end, 0x30, &in, &in_end) != 0 || p != end) return -1;
        while (in < in_end) {
            if (der_read(&in, in_end, &tag, &v, &l) != 0) return -1;
            if (tag == 0x82 && c->ndns < XCERT_MAX_SAN) {
                c->dns[c->ndns].p = v;
                c->dns[c->ndns].n = l;
                c->ndns++;
            } else if (tag == 0x87 && l == 4 && c->nip4 < 8) {
                memcpy(c->ip4[c->nip4].b, v, 4);
                c->nip4++;
            }
        }
    } else if (OIDEQ(oid, ol, OID_POL) || OIDEQ(oid, ol, OID_SKI) || OIDEQ(oid, ol, OID_AKI) ||
               OIDEQ(oid, ol, OID_CRLDP)) {
        ;                                   /* understood well enough: nothing to enforce */
    } else {
        known = 0;
    }
    if (!known && critical) c->unknown_critical = 1;
    return 0;
}

int xcert_parse(XCert *c, const u8 *der, size_t len, const char **err)
{
    const u8 *p = der, *end = der + len, *cert, *cert_end, *tbs, *tbs_end, *v, *start, *oid, *alg, *alg_end;
    const u8 *outer_alg_start, *bits;
    size_t l, ol, bl;
    int tag, unused;

    *err = "corrupt certificate";
    if (x509_parse_leaf(der, len, &c->key, err) != 0) return -1;
    *err = "corrupt certificate";
    c->der = der;
    c->der_len = len;
    if (der_enter(&p, end, 0x30, &cert, &cert_end) != 0 || p != end) return -1;
    start = cert;
    if (der_expect(&cert, cert_end, 0x30, &tbs, &l) != 0) return -1;
    tbs_end = tbs + l;
    c->tbs = start;
    c->tbs_len = (size_t)(cert - start);
    outer_alg_start = cert;
    if (der_enter(&cert, cert_end, 0x30, &alg, &alg_end) != 0) return -1;
    {
        size_t alg_tlv = (size_t)(cert - outer_alg_start);

        if (der_expect(&alg, alg_end, 0x06, &oid, &ol) != 0) return -1;
        c->sig_alg = sig_alg_from_oid(oid, ol);
        c->sig_unsupported = c->sig_alg == XSIG_NONE;
        if (der_bitstring(&cert, cert_end, &bits, &bl, &unused) != 0 || unused != 0 || cert != cert_end) return -1;
        c->sig = bits;
        c->sig_len = bl;

        /* inside the TBS: [0] version, serial, signature (must repeat the outer), issuer, validity, subject, spki */
        c->version = 1;
        if (tbs < tbs_end && *tbs == 0xa0) {
            const u8 *vi, *vi_end;

            if (der_enter(&tbs, tbs_end, 0xa0, &vi, &vi_end) != 0 || der_int(&vi, vi_end, &v, &l) != 0 || l != 1) return -1;
            c->version = v[0] + 1;
        }
        if (der_expect(&tbs, tbs_end, 0x02, &v, &l) != 0) return -1;
        {
            const u8 *s2 = tbs;

            if (der_expect(&tbs, tbs_end, 0x30, &v, &l) != 0) return -1;
            if ((size_t)(tbs - s2) != alg_tlv || memcmp(s2, outer_alg_start, alg_tlv) != 0) {
                *err = "certificate signature algorithms disagree";
                return -1;
            }
        }
    }
    start = tbs;
    if (der_expect(&tbs, tbs_end, 0x30, &v, &l) != 0) return -1;
    c->issuer = start;
    c->issuer_len = (size_t)(tbs - start);
    if (der_expect(&tbs, tbs_end, 0x30, &v, &l) != 0) return -1;             /* validity */
    start = tbs;
    if (der_expect(&tbs, tbs_end, 0x30, &v, &l) != 0) return -1;
    c->subject = start;
    c->subject_len = (size_t)(tbs - start);
    if (der_expect(&tbs, tbs_end, 0x30, &v, &l) != 0) return -1;             /* spki */
    while (tbs < tbs_end) {
        if (der_read(&tbs, tbs_end, &tag, &v, &l) != 0) return -1;
        if (tag == 0xa3) {
            const u8 *exts = v, *exts_end = v + l, *seq, *seq_end;

            if (c->version < 3) return -1;
            if (der_enter(&exts, exts_end, 0x30, &seq, &seq_end) != 0 || exts != exts_end) return -1;
            while (seq < seq_end) {
                const u8 *ext, *ext_end, *eo, *ev;
                size_t eol, evl, bl2;
                int critical = 0;

                if (der_enter(&seq, seq_end, 0x30, &ext, &ext_end) != 0) return -1;
                if (der_expect(&ext, ext_end, 0x06, &eo, &eol) != 0) return -1;
                if (ext < ext_end && *ext == 0x01) {
                    if (der_expect(&ext, ext_end, 0x01, &ev, &bl2) != 0 || bl2 != 1) return -1;
                    critical = ev[0] != 0;
                }
                if (der_expect(&ext, ext_end, 0x04, &ev, &evl) != 0 || ext != ext_end) return -1;
                if (parse_extension(c, eo, eol, critical, ev, evl) != 0) {
                    *err = "malformed certificate extension";
                    return -1;
                }
            }
        }
    }
    *err = "ok";
    return 0;
}

/* ---------------------------------------------------------------- host names */

static int lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

static int name_matches(const u8 *pat, size_t pn, const char *host, size_t hn)
{
    size_t i;

    if (pn == 0 || hn == 0) return 0;
    if (pn >= 2 && pat[0] == '*' && pat[1] == '.') {
        /* one whole left-most label only: *.example.com matches a.example.com, not a.b.example.com
         * nor example.com, and the remainder must still hold at least two labels */
        size_t first = 0;
        size_t dots = 0;

        while (first < hn && host[first] != '.') first++;
        if (first == 0 || first >= hn) return 0;
        for (i = 2; i < pn; i++) if (pat[i] == '.') dots++;
        if (dots == 0) return 0;
        pat += 1;
        pn -= 1;
        host += first;
        hn -= first;
    }
    if (pn != hn) return 0;
    for (i = 0; i < pn; i++) {
        if (lower(pat[i]) != lower((unsigned char)host[i])) return 0;
    }
    return 1;
}

static int parse_ipv4(const char *s, u8 out[4])
{
    int i, v;

    for (i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 255) return 0;
            s++;
        }
        out[i] = (u8)v;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s == '\0';
}

int xcert_matches_host(const XCert *c, const char *host)
{
    size_t hn = strlen(host);
    u8 ip[4];
    int i;

    if (hn > 0 && host[hn - 1] == '.') hn--;
    if (parse_ipv4(host, ip)) {
        for (i = 0; i < c->nip4; i++) {
            if (memcmp(c->ip4[i].b, ip, 4) == 0) return 1;
        }
        return 0;
    }
    for (i = 0; i < c->ndns; i++) {
        if (name_matches(c->dns[i].p, c->dns[i].n, host, hn)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- signatures */

static int verify_sig(const XCert *subj, const XCert *issuer)
{
    u8 h[64];
    size_t hl;

    switch (subj->sig_alg) {
    case XSIG_RSA_SHA256:
    case XSIG_RSA_SHA384:
    case XSIG_RSA_SHA512:
        if (issuer->key.key_type != X509_KEY_RSA) return -1;
        return rsa_verify(&issuer->key.rsa,
                          subj->sig_alg == XSIG_RSA_SHA256 ? RSA_SHA256 : subj->sig_alg == XSIG_RSA_SHA384 ? RSA_SHA384 : RSA_SHA512,
                          subj->tbs, subj->tbs_len, subj->sig, subj->sig_len);
    case XSIG_ECDSA_SHA256:
    case XSIG_ECDSA_SHA384:
    case XSIG_ECDSA_SHA512: {
        const ec_curve *cv;
        bn r, s;
        int rc = -1;

        if (issuer->key.key_type != X509_KEY_EC) return -1;
        cv = ec_curve_get(issuer->key.curve);
        if (cv == NULL) return -1;
        if (subj->sig_alg == XSIG_ECDSA_SHA256) { sha256(subj->tbs, subj->tbs_len, h); hl = 32; }
        else if (subj->sig_alg == XSIG_ECDSA_SHA384) { sha384(subj->tbs, subj->tbs_len, h); hl = 48; }
        else { sha512(subj->tbs, subj->tbs_len, h); hl = 64; }
        bn_init(&r);
        bn_init(&s);
        if (der_ecdsa_sig(subj->sig, subj->sig_len, &r, &s) == 0) {
            rc = ecdsa_verify(cv, &issuer->key.ec_pub, h, hl, &r, &s);
        }
        bn_free(&r);
        bn_free(&s);
        return rc;
    }
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------- trust store */

void trust_init(TrustStore *ts)
{
    memset(ts, 0, sizeof(*ts));
}

void trust_free(TrustStore *ts)
{
    int i;

    for (i = 0; i < ts->n; i++) {
        xcert_free(&ts->a[i].cert);
        free(ts->a[i].der);
    }
    free(ts->a);
    memset(ts, 0, sizeof(*ts));
}

int trust_add_der(TrustStore *ts, const u8 *der, size_t len)
{
    TrustAnchor *a;
    const char *err;
    int i;

    for (i = 0; i < ts->n; i++) {
        if (ts->a[i].der_len == len && memcmp(ts->a[i].der, der, len) == 0) return 0;
    }
    if (ts->n >= ts->cap) {
        int ncap = ts->cap ? ts->cap * 2 : 32;
        TrustAnchor *na = (TrustAnchor *)realloc(ts->a, (size_t)ncap * sizeof(TrustAnchor));

        if (na == NULL) return -1;
        ts->a = na;
        ts->cap = ncap;
    }
    a = &ts->a[ts->n];
    memset(a, 0, sizeof(*a));
    a->der = (u8 *)malloc(len > 0 ? len : 1);
    if (a->der == NULL) return -1;
    memcpy(a->der, der, len);
    a->der_len = len;
    xcert_init(&a->cert);
    if (xcert_parse(&a->cert, a->der, len, &err) != 0) {
        xcert_free(&a->cert);
        free(a->der);
        memset(a, 0, sizeof(*a));
        return -1;
    }
    ts->n++;
    return 1;
}

int trust_add_pem(TrustStore *ts, const char *text, size_t len)
{
    static const char BEGIN[] = "-----BEGIN CERTIFICATE-----";
    static const char END[] = "-----END CERTIFICATE-----";
    const char *p = text, *end = text + len, *b, *e, *q;
    int added = 0;
    char *clean;
    u8 *der;
    size_t n, dl;

    while (p < end) {
        b = NULL;
        for (q = p; q + sizeof(BEGIN) - 1 <= end; q++) {
            if (*q == '-' && memcmp(q, BEGIN, sizeof(BEGIN) - 1) == 0) {
                b = q + sizeof(BEGIN) - 1;
                break;
            }
        }
        if (b == NULL) break;
        e = NULL;
        for (q = b; q + sizeof(END) - 1 <= end; q++) {
            if (*q == '-' && memcmp(q, END, sizeof(END) - 1) == 0) {
                e = q;
                break;
            }
        }
        if (e == NULL) break;
        p = e + sizeof(END) - 1;
        if ((size_t)(e - b) > 32768) continue;
        clean = (char *)malloc((size_t)(e - b) + 1);
        der = (u8 *)malloc((size_t)(e - b) / 4 * 3 + 4);
        if (clean == NULL || der == NULL) {
            free(clean);
            free(der);
            break;
        }
        n = 0;
        for (q = b; q < e; q++) {
            if (*q != ' ' && *q != '\r' && *q != '\n' && *q != '\t') clean[n++] = *q;
        }
        clean[n] = '\0';
        if (n > 0 && n % 4 == 0 && base64_decode(clean, der, &dl) == 0 && dl > 0 && trust_add_der(ts, der, dl) == 1) {
            added++;
        }
        free(clean);
        free(der);
    }
    return added;
}

/* ---------------------------------------------------------------- chain building */

typedef struct {
    const TrustStore *ts;
    XCert xs[CHAIN_MAX_CERTS];
    int parsed[CHAIN_MAX_CERTS];
    int used[CHAIN_MAX_CERTS];
    int n;
    const der_time *now;
    char reason[200];
    int reason_rank;
} Ctx;

static void note(Ctx *c, int rank, const char *msg)
{
    if (rank > c->reason_rank) {
        c->reason_rank = rank;
        strncpy(c->reason, msg, sizeof(c->reason) - 1);
        c->reason[sizeof(c->reason) - 1] = '\0';
    }
}

static int same_name(const u8 *a, size_t al, const u8 *b, size_t bl)
{
    return al == bl && memcmp(a, b, al) == 0;
}

/* NULL if the certificate is within its validity period (or dates are not being checked). */
static const char *date_problem(const Ctx *c, const XCert *x)
{
    if (c->now == NULL) return NULL;
    if (der_time_cmp(c->now, &x->key.not_before) < 0) {
        return "a certificate in the chain is not valid yet: check that the computer's date and time are right";
    }
    if (der_time_cmp(c->now, &x->key.not_after) > 0) {
        return "a certificate in the chain has expired (or the computer's date and time are wrong)";
    }
    return NULL;
}

static int weak_key(const XCert *x)
{
    return x->key.key_type == X509_KEY_RSA && rsa_size_bytes(&x->key.rsa.n) * 8 < 2048;
}

static int build(Ctx *c, int cur, int depth)
{
    const XCert *x = &c->xs[cur];
    const TrustStore *ts = c->ts;
    const char *why;
    int i, j;

    for (i = 0; i < ts->n; i++) {
        if (ts->a[i].der_len == x->der_len && memcmp(ts->a[i].der, x->der, x->der_len) == 0) return 0;
    }
    for (i = 0; i < ts->n; i++) {
        const XCert *a = &ts->a[i].cert;

        if (!same_name(a->subject, a->subject_len, x->issuer, x->issuer_len)) continue;
        if (a->has_bc && !a->is_ca) { note(c, 1, "the issuer in the trust store is not a CA"); continue; }
        if (a->has_pathlen && depth > a->pathlen) { note(c, 1, "the certificate chain is longer than its CA allows"); continue; }
        why = date_problem(c, a);
        if (why != NULL) { note(c, 3, why); continue; }
        if (x->sig_unsupported) { note(c, 2, "a certificate uses an unsupported or insecure signature algorithm"); continue; }
        if (verify_sig(x, a) == 0) return 0;
        note(c, 2, "a certificate signature in the chain does not verify");
    }
    if (depth + 1 > CHAIN_MAX_DEPTH) return -1;
    for (j = 1; j < c->n; j++) {
        const XCert *ic;

        if (!c->parsed[j] || c->used[j] || j == cur) continue;
        ic = &c->xs[j];
        if (!same_name(ic->subject, ic->subject_len, x->issuer, x->issuer_len)) continue;
        if (!ic->has_bc || !ic->is_ca || (ic->has_ku && !(ic->ku & 0x04))) {
            note(c, 1, "an intermediate certificate is not allowed to sign certificates");
            continue;
        }
        if (ic->unknown_critical) { note(c, 1, "a certificate has a critical extension this client does not support"); continue; }
        if (ic->has_pathlen && depth > ic->pathlen) { note(c, 1, "the certificate chain is longer than its CA allows"); continue; }
        why = date_problem(c, ic);
        if (why != NULL) { note(c, 3, why); continue; }
        if (weak_key(ic)) { note(c, 1, "a certificate in the chain has a weak key"); continue; }
        if (x->sig_unsupported) { note(c, 2, "a certificate uses an unsupported or insecure signature algorithm"); continue; }
        if (verify_sig(x, ic) != 0) { note(c, 2, "a certificate signature in the chain does not verify"); continue; }
        c->used[j] = 1;
        if (build(c, j, depth + 1) == 0) {
            c->used[j] = 0;
            return 0;
        }
        c->used[j] = 0;
    }
    return -1;
}

int chain_verify(const TrustStore *ts, const ChainCert *certs, int n, const char *host, const der_time *now,
                 char *err, size_t errcap)
{
    Ctx *c;
    const char *e, *why;
    int i, rc = -1;

    if (errcap == 0) return -1;
    err[0] = '\0';
    if (n < 1 || n > CHAIN_MAX_CERTS) {
        strncpy(err, "the server sent no certificates (or too many)", errcap - 1);
        err[errcap - 1] = '\0';
        return -1;
    }
    c = (Ctx *)calloc(1, sizeof(Ctx));
    if (c == NULL) {
        strncpy(err, "out of memory", errcap - 1);
        err[errcap - 1] = '\0';
        return -1;
    }
    c->ts = ts;
    c->n = n;
    c->now = now;
    for (i = 0; i < n; i++) {
        xcert_init(&c->xs[i]);
        c->parsed[i] = xcert_parse(&c->xs[i], certs[i].der, certs[i].len, &e) == 0;
        if (i == 0 && !c->parsed[0]) {
            sprintf(c->reason, "the server's certificate could not be read (%.100s)", e);
            goto done;
        }
    }
    {
        const XCert *leaf = &c->xs[0];

        if (!xcert_matches_host(leaf, host)) {
            sprintf(c->reason, "the server's certificate is not valid for the name \"%.100s\"", host);
            goto done;
        }
        if (leaf->unknown_critical) {
            strcpy(c->reason, "the server's certificate has a critical extension this client does not support");
            goto done;
        }
        if (leaf->has_eku && !leaf->eku_server) {
            strcpy(c->reason, "the server's certificate is not meant for TLS servers");
            goto done;
        }
        if (leaf->has_ku && !(leaf->ku & 0x80)) {
            strcpy(c->reason, "the server's certificate may not be used for signatures");
            goto done;
        }
        if (weak_key(leaf)) {
            strcpy(c->reason, "the server's certificate has a weak key");
            goto done;
        }
        why = date_problem(c, leaf);
        if (why != NULL) {
            strncpy(c->reason, why, sizeof(c->reason) - 1);
            goto done;
        }
    }
    if (build(c, 0, 0) == 0) {
        rc = 0;
        goto done;
    }
    if (c->reason[0] == '\0') {
        strcpy(c->reason, "the server's certificate is not signed by a trusted authority");
    }
done:
    if (rc != 0) {
        strncpy(err, c->reason, errcap - 1);
        err[errcap - 1] = '\0';
    }
    for (i = 0; i < n; i++) {
        xcert_free(&c->xs[i]);
    }
    free(c);
    return rc;
}
