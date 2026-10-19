#!/usr/bin/env python3
"""Scripted HTTP/1.1 server for http_smoke: every response shape the client must cope with.

usage: http_smoke_server.py PORT [CERT KEY]    (with CERT/KEY it speaks TLS 1.2, ECDHE-RSA-AES128-GCM)
Serves one connection per thread, keep-alive until the client closes or a route says otherwise.
"""
import socket, ssl, sys, threading, time

PORT = int(sys.argv[1])
CERT = sys.argv[2] if len(sys.argv) > 2 else None
KEY = sys.argv[3] if len(sys.argv) > 3 else None
BIG = bytes((i * 7 + 3) & 0xff for i in range(300 * 1024))


def read_request(f):
    line = f.readline()
    if not line:
        return None
    method, path, _ = line.decode('latin1').split(' ', 2)
    headers = {}
    while True:
        h = f.readline().decode('latin1')
        if h in ('\r\n', '\n', ''):
            break
        k, v = h.split(':', 1)
        headers[k.strip().lower()] = v.strip()
    body = b''
    if 'content-length' in headers:
        body = f.read(int(headers['content-length']))
    return method, path, headers, body


def respond(conn, status, headers, body):
    out = ('HTTP/1.1 %s\r\n' % status).encode()
    for k, v in headers:
        out += ('%s: %s\r\n' % (k, v)).encode()
    out += b'\r\n' + body
    conn.sendall(out)


def chunked(conn, chunks, trailer=True):
    conn.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n')
    for c in chunks:
        conn.sendall(b'%x;note=x\r\n' % len(c) + c + b'\r\n')
        time.sleep(0.01)
    conn.sendall(b'0\r\n' + (b'X-Trailer: yes\r\n' if trailer else b'') + b'\r\n')


def handle(conn):
    f = conn.makefile('rb')
    try:
        while True:
            req = read_request(f)
            if req is None:
                return
            method, path, headers, body = req
            if path == '/len':
                b = b'hello world'
                respond(conn, '200 OK', [('Content-Length', str(len(b))), ('X-RateLimit-Remaining', '4')],
                        b if method != 'HEAD' else b'')
            elif path == '/chunked':
                chunked(conn, [b'alpha ', b'beta ', b'gamma'])
            elif path == '/bigchunked':
                chunked(conn, [BIG[i:i + 4096] for i in range(0, 200 * 1024, 4096)], trailer=False)
            elif path == '/big':
                respond(conn, '200 OK', [('Content-Length', str(len(BIG)))], BIG)
            elif path == '/echo':
                import json
                payload = json.dumps({'method': method, 'len': len(body), 'body': body.decode('utf-8', 'replace'),
                                      'ctype': headers.get('content-type', '')}).encode()
                respond(conn, '200 OK', [('Content-Length', str(len(payload))), ('Content-Type', 'application/json')], payload)
            elif path == '/headers':
                text = ''.join('%s=%s\n' % kv for kv in sorted(headers.items())).encode()
                respond(conn, '200 OK', [('Content-Length', str(len(text)))], text)
            elif path == '/100':
                conn.sendall(b'HTTP/1.1 100 Continue\r\n\r\n')
                time.sleep(0.02)
                respond(conn, '201 Created', [('Content-Length', '2')], b'ok')
            elif path == '/204':
                respond(conn, '204 No Content', [], b'')
            elif path == '/429':
                b = b'{"message":"You are being rate limited.","retry_after":0.5,"global":false}'
                respond(conn, '429 Too Many Requests', [('Content-Length', str(len(b))), ('Retry-After', '1')], b)
            elif path == '/close':
                conn.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nread until the connection closes')
                return
            else:
                respond(conn, '404 Not Found', [('Content-Length', '0')], b'')
    except (ConnectionError, ssl.SSLError, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT))
srv.listen(8)
ctx = None
if CERT:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers('ECDHE-RSA-AES128-GCM-SHA256')
    ctx.load_cert_chain(CERT, KEY)
while True:
    c, _ = srv.accept()
    if ctx:
        try:
            c = ctx.wrap_socket(c, server_side=True)
        except (ssl.SSLError, OSError):
            continue
    threading.Thread(target=handle, args=(c,), daemon=True).start()
