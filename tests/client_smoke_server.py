#!/usr/bin/env python3
"""A scripted fake Discord (REST and gateway on one port) for client_smoke, built on the `websockets` library.

usage: client_smoke_server.py PORT [CERT KEY]    (REST on PORT, the gateway WebSocket on PORT+1)
REST (the process_request hook answers anything that is not a WebSocket upgrade):
  GET  /api/v10/users/@me               needs "Authorization: Bot goodtoken" (else 401)
  GET  /api/v10/channels/200/messages   three older messages, newest first
  GET  /api/v10/channels/201/messages   403 Missing Access
  POST /api/v10/channels/200/messages   echoes the message through the gateway; special texts:
         "ratelimit" -> 429 (retry_after 0.3) once, then it works; "fail" -> 400; "kick" -> sends the
         gateway RECONNECT (op 7) to every connected client, which must resume and receive a missed message
Gateway (any WebSocket upgrade): HELLO, IDENTIFY or RESUME, READY + GUILD_CREATE + a message, heartbeat ACKs.
"""
import asyncio, json, ssl, sys
from http import HTTPStatus
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

PORT = int(sys.argv[1])
CERT = sys.argv[2] if len(sys.argv) > 2 else None
KEY = sys.argv[3] if len(sys.argv) > 3 else None
TOKEN = 'goodtoken'
SCHEME = 'wss' if CERT else 'ws'
clients = set()
state = {'seq': 100, 'next_id': 500, 'limited': False, 'missed': False}


def msg(mid, content, author='7', name='someone', channel='200', mentions=None):
    return {'id': str(mid), 'channel_id': channel, 'guild_id': '100', 'content': content,
            'timestamp': '2026-10-19T10:00:00.000000+00:00', 'edited_timestamp': None, 'type': 0,
            'author': {'id': author, 'username': name, 'global_name': None}, 'mentions': mentions or [],
            'attachments': [], 'embeds': []}


def frame(op, d=None, s=None, t=None):
    return json.dumps({'op': op, 'd': d, 's': s, 't': t})


async def broadcast(t, d):
    state['seq'] += 1
    for ws in list(clients):
        try:
            await ws.send(frame(0, d, state['seq'], t))
        except Exception:
            pass


async def read_request(reader):
    line = await reader.readline()
    if not line:
        return None
    method, target, _ = line.decode('latin1').split(' ', 2)
    headers = {}
    while True:
        h = (await reader.readline()).decode('latin1')
        if h in ('\r\n', '\n', ''):
            break
        k, v = h.split(':', 1)
        headers[k.strip().lower()] = v.strip()
    body = b''
    if 'content-length' in headers:
        body = await reader.readexactly(int(headers['content-length']))
    return method, target, headers, body


async def reply(writer, status, obj, extra=None, close=False):
    body = json.dumps(obj).encode()
    out = ('HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n' % (status.value, status.phrase, len(body)))
    for k, v in (extra or []):
        out += '%s: %s\r\n' % (k, v)
    if close:
        out += 'Connection: close\r\n'
    writer.write(out.encode() + b'\r\n' + body)
    await writer.drain()


async def rest_conn(reader, writer):
    try:
        while True:
            req = await read_request(reader)
            if req is None:
                break
            method, target, headers, body = req
            state['requests'] = state.get('requests', 0) + 1
            path = target.split('?')[0]
            if headers.get('authorization', '') != 'Bot ' + TOKEN:
                await reply(writer, HTTPStatus.UNAUTHORIZED, {'message': '401: Unauthorized', 'code': 0})
            elif method == 'GET' and path == '/api/v10/users/@me':
                await reply(writer, HTTPStatus.OK, {'id': '42', 'username': 'Kallisti-bot', 'bot': True})
            elif method == 'GET' and path == '/api/v10/channels/200/messages':
                mentions = [{'id': '42', 'username': 'Kallisti-bot'}]
                await reply(writer, HTTPStatus.OK, [msg(12, 'newest <@42>', mentions=mentions), msg(11, 'middle'), msg(10, 'oldest')],
                            [('X-RateLimit-Remaining', '4'), ('X-RateLimit-Reset-After', '1.0')])
            elif method == 'GET' and path == '/api/v10/channels/201/messages':
                await reply(writer, HTTPStatus.FORBIDDEN, {'message': 'Missing Access', 'code': 50001})
            elif method == 'POST' and path == '/api/v10/channels/200/messages':
                content = json.loads(body)['content']
                if content == 'ratelimit' and not state['limited']:
                    state['limited'] = True
                    await reply(writer, HTTPStatus.TOO_MANY_REQUESTS,
                                {'message': 'You are being rate limited.', 'retry_after': 0.3, 'global': False},
                                [('Retry-After', '1')])
                elif content == 'fail':
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'Cannot send an empty message', 'code': 50006})
                else:
                    state['next_id'] += 1
                    m = msg(state['next_id'], content, author='42', name='Kallisti-bot')
                    if content == 'kick':
                        state['missed'] = True
                    await reply(writer, HTTPStatus.OK, m)
                    await broadcast('MESSAGE_CREATE', m)
                    if content == 'kick':
                        for ws in list(clients):
                            await ws.send(frame(7))
            else:
                await reply(writer, HTTPStatus.NOT_FOUND, {'message': '404: Not Found', 'code': 0})
            if headers.get('connection', '').lower() == 'close':
                break
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writer.close()


async def handler(ws):
    clients.add(ws)
    try:
        await ws.send(frame(10, {'heartbeat_interval': 1000}))
        first = True
        async for raw in ws:
            m = json.loads(raw)
            if m['op'] == 1:
                await ws.send(frame(11))
            elif m['op'] == 2:
                d = m['d']
                assert d['token'] == TOKEN and d['intents'] == (1 | (1 << 9) | (1 << 15)), d
                state['seq'] = 100
                await ws.send(frame(0, {'session_id': 'sess-1', 'resume_gateway_url': '%s://localhost:%d' % (SCHEME, PORT + 1),
                                        'user': {'id': '42', 'username': 'Kallisti-bot'},
                                        'guilds': [{'id': '100', 'unavailable': True}]}, 101, 'READY'))
                await ws.send(frame(0, {'id': '100', 'name': 'Test Guild', 'channels': [
                    {'id': '210', 'name': 'Text', 'type': 4, 'position': 0},
                    {'id': '200', 'name': 'general', 'type': 0, 'position': 0, 'parent_id': '210'},
                    {'id': '201', 'name': 'secret', 'type': 0, 'position': 1, 'parent_id': '210'},
                    {'id': '202', 'name': 'voice', 'type': 2, 'position': 2}]}, 102, 'GUILD_CREATE'))
                await ws.send(frame(0, msg(20, 'hi <@42> from <#201>', mentions=[{'id': '42', 'username': 'Kallisti-bot'}]),
                                    103, 'MESSAGE_CREATE'))
                state['seq'] = 103
            elif m['op'] == 6:
                d = m['d']
                assert d['session_id'] == 'sess-1' and d['token'] == TOKEN, d
                if state['missed']:
                    state['seq'] += 1
                    await ws.send(frame(0, msg(30, 'sent while you were away'), state['seq'], 'MESSAGE_CREATE'))
                await ws.send(frame(0, {}, state['seq'] + 1, 'RESUMED'))
                state['seq'] += 1
    except ConnectionClosed:
        pass
    finally:
        clients.discard(ws)


async def main():
    ctx = tls_context()
    rest = await asyncio.start_server(rest_conn, '127.0.0.1', PORT, ssl=ctx)
    async with serve(handler, '127.0.0.1', PORT + 1, ssl=ctx, compression=None):
        await asyncio.get_running_loop().create_future()


def tls_context():
    if not CERT:
        return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers('ECDHE-RSA-AES128-GCM-SHA256')
    ctx.load_cert_chain(CERT, KEY)
    return ctx

asyncio.run(main())
