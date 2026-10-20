#!/usr/bin/env python3
"""Proxies and an echo server for the proxy tests, all on 127.0.0.1.

usage: proxy_server.py BASE_PORT LOG_FILE
  BASE+0  HTTP CONNECT proxy, no login        BASE+2  SOCKS5 proxy, no login
  BASE+1  HTTP CONNECT proxy, login bob/s3cret  BASE+3  SOCKS5 proxy, login bob/s3cret
  BASE+4  an echo server: says "hello from echo\\n" the moment a client connects, then echoes what it is sent
Policy: a request for port BASE+8 is forbidden (HTTP 403 / SOCKS rule-set refusal); the host "nowhere.invalid" cannot
be found (HTTP 502 / SOCKS 4); anything that will not connect is a 502 / SOCKS 5.  Every tunnel request is appended
to LOG_FILE as "<proxy> CONNECT host:port [user=NAME]".
"""
import asyncio, base64, socket, struct, sys

BASE = int(sys.argv[1])
LOG = sys.argv[2]
USER, PASSWORD = 'bob', 's3cret'


def log(line):
    with open(LOG, 'a') as f:
        f.write(line + '\n')


async def pipe(r, w):
    try:
        while True:
            data = await r.read(65536)
            if not data:
                break
            w.write(data)
            await w.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            w.close()
        except Exception:
            pass


async def tunnel(cr, cw, host, port):
    """Connects to the target; returns (reader, writer) or an error code (502 unreachable / 'noname' / 'refused')."""
    if host == 'nowhere.invalid':
        return 'noname'
    try:
        return await asyncio.open_connection(host, port)
    except OSError:
        return 'refused'


async def http_proxy(reader, writer, name, auth):
    try:
        head = await reader.readuntil(b'\r\n\r\n')
        lines = head.decode('latin1').split('\r\n')
        method, target, _ = lines[0].split(' ', 2)
        hdrs = {l.split(':', 1)[0].lower(): l.split(':', 1)[1].strip() for l in lines[1:] if ':' in l}
        if method != 'CONNECT':
            writer.write(b'HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n')
            await writer.drain()
            return
        host, port = target.rsplit(':', 1)
        port = int(port)
        who = ''
        if auth:
            want = 'Basic ' + base64.b64encode(('%s:%s' % (USER, PASSWORD)).encode()).decode()
            if hdrs.get('proxy-authorization') != want:
                log('%s CONNECT %s:%d denied-login' % (name, host, port))
                writer.write(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="x"\r\nContent-Length: 0\r\n\r\n')
                await writer.drain()
                return
            who = ' user=' + USER
        log('%s CONNECT %s:%d%s' % (name, host, port, who))
        if port == BASE + 8:
            writer.write(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n')
            await writer.drain()
            return
        res = await tunnel(reader, writer, host, port)
        if isinstance(res, str):
            writer.write(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n')
            await writer.drain()
            return
        tr, tw = res
        writer.write(b'HTTP/1.1 200 Connection established\r\n\r\n')
        await writer.drain()
        await asyncio.gather(pipe(reader, tw), pipe(tr, writer))
    except (asyncio.IncompleteReadError, ConnectionError, ValueError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def socks_proxy(reader, writer, name, auth):
    try:
        ver, nm = struct.unpack('BB', await reader.readexactly(2))
        methods = await reader.readexactly(nm)
        if ver != 5:
            return
        who = ''
        if auth:
            if 2 not in methods:
                writer.write(b'\x05\xff')
                await writer.drain()
                return
            writer.write(b'\x05\x02')
            await writer.drain()
            v, ul = struct.unpack('BB', await reader.readexactly(2))
            u = (await reader.readexactly(ul)).decode()
            pl = (await reader.readexactly(1))[0]
            p = (await reader.readexactly(pl)).decode()
            if (u, p) != (USER, PASSWORD):
                log('%s login-denied' % name)
                writer.write(b'\x01\x01')
                await writer.drain()
                return
            writer.write(b'\x01\x00')
            await writer.drain()
            who = ' user=' + USER
        else:
            writer.write(b'\x05\x00')
            await writer.drain()
        v, cmd, _, atyp = struct.unpack('BBBB', await reader.readexactly(4))
        if atyp == 1:
            host = socket.inet_ntoa(await reader.readexactly(4))
        elif atyp == 3:
            host = (await reader.readexactly((await reader.readexactly(1))[0])).decode()
        else:
            writer.write(b'\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00')
            await writer.drain()
            return
        port = struct.unpack('>H', await reader.readexactly(2))[0]
        log('%s CONNECT %s:%d%s' % (name, host, port, who))
        if cmd != 1:
            writer.write(b'\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00')
            await writer.drain()
            return
        if port == BASE + 8:
            writer.write(b'\x05\x02\x00\x01\x00\x00\x00\x00\x00\x00')
            await writer.drain()
            return
        res = await tunnel(reader, writer, host, port)
        if isinstance(res, str):
            writer.write(b'\x05' + (b'\x04' if res == 'noname' else b'\x05') + b'\x00\x01\x00\x00\x00\x00\x00\x00')
            await writer.drain()
            return
        tr, tw = res
        # reply with a domain-name bound address (exercises the variable-length reply parser)
        writer.write(b'\x05\x00\x00\x03\x09localhost\x1f\x90')
        await writer.drain()
        await asyncio.gather(pipe(reader, tw), pipe(tr, writer))
    except (asyncio.IncompleteReadError, ConnectionError, UnicodeDecodeError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def echo(reader, writer):
    try:
        writer.write(b'hello from echo\n')
        await writer.drain()
        while True:
            data = await reader.read(4096)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except ConnectionError:
        pass
    finally:
        writer.close()


async def main():
    await asyncio.start_server(lambda r, w: http_proxy(r, w, 'http-open', False), '127.0.0.1', BASE)
    await asyncio.start_server(lambda r, w: http_proxy(r, w, 'http-auth', True), '127.0.0.1', BASE + 1)
    await asyncio.start_server(lambda r, w: socks_proxy(r, w, 'socks-open', False), '127.0.0.1', BASE + 2)
    await asyncio.start_server(lambda r, w: socks_proxy(r, w, 'socks-auth', True), '127.0.0.1', BASE + 3)
    await asyncio.start_server(echo, '127.0.0.1', BASE + 4)
    await asyncio.get_running_loop().create_future()

asyncio.run(main())
