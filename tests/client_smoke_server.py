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
import asyncio, json, os, ssl, sys
from http import HTTPStatus
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

PORT = int(sys.argv[1])
CERT = sys.argv[2] if len(sys.argv) > 2 else None
KEY = sys.argv[3] if len(sys.argv) > 3 else None
TOKEN = 'goodtoken'
SCHEME = 'wss' if CERT else 'ws'
SCHEME_HTTP = 'https' if CERT else 'http'
clients = set()
JPG = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'jpg', 'big_420.jpg'), 'rb').read()
MANY = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gif', 'a_many.gif'), 'rb').read()
ONE = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gif', 'a_one.gif'), 'rb').read()
GIF = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gif', 'anim.gif'), 'rb').read()
EMOJI = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'img', 't6_d8_i0_13x7.png'), 'rb').read()
PNG = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'img', 'big_rgb_interlaced.png'), 'rb').read()
state = {'seq': 100, 'next_id': 500, 'limited': False, 'missed': False}


def msg(mid, content, author='7', name='someone', channel='200', mentions=None, guild='100'):
    m = {'id': str(mid), 'channel_id': channel, 'content': content,
            'timestamp': '2026-10-19T10:00:00.000000+00:00', 'edited_timestamp': None, 'type': 0,
            'author': {'id': author, 'username': name, 'global_name': None}, 'mentions': mentions or [],
            'attachments': [], 'embeds': []}
    if guild:
        m['guild_id'] = guild
    return m


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
            query = target.split('?', 1)[1] if '?' in target else ''
            if path.startswith('/img/'):                  # the media proxy: no authorization, and it wants format=png
                if 'authorization' in headers:
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'a token went to the CDN'})
                elif path == '/img/redirect':
                    writer.write(b'HTTP/1.1 302 Found\r\nLocation: /img/cat.png?ex=2&format=png\r\nContent-Length: 0\r\n\r\n')
                    await writer.drain()
                elif path == '/img/missing':
                    await reply(writer, HTTPStatus.NOT_FOUND, {'message': 'no such file'})
                elif path == '/img/notpng':
                    writer.write(b'HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\nContent-Length: 11\r\n\r\nnot a png!!')
                    await writer.drain()
                elif path == '/img/nojpeg':          # a proxy that will not make JPEGs: webp for them, a PNG for format=png
                    if 'format=jpeg' in query:
                        writer.write(b'HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\nContent-Length: 12\r\n\r\nRIFFxxxxWEBP')
                    else:
                        writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n' % len(PNG)).encode() + PNG)
                    await writer.drain()
                elif path == '/img/closeme':          # says it will close: the client must not keep the connection
                    writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: %d\r\n\r\n' % len(PNG)).encode() + PNG)
                    await writer.drain()
                    break
                elif path == '/img/slam':             # closes right after the reply without saying so
                    writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n' % len(PNG)).encode() + PNG)
                    await writer.drain()
                    break
                elif path.startswith('/img/emoji/'):    # custom emoji pictures
                    writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n' % len(EMOJI)).encode() + EMOJI)
                    await writer.drain()
                elif path == '/img/file.bin':           # a download: handed back untouched, and the address must be as given
                    if query != 'sig=zz&format=png':
                        await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'the address was changed: ' + query})
                    else:
                        data = bytes((i * 7 + i // 256) & 255 for i in range(70000))
                        writer.write(('HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n' % len(data)).encode() + data)
                        await writer.drain()
                elif path == '/img/plainfetch':        # an outside site: it must get the address untouched
                    if query != 'token=abc':
                        await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'the address was changed: ' + query})
                    else:
                        writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n' % len(PNG)).encode() + PNG)
                        await writer.drain()
                elif path in ('/img/animsized', '/img/many.gif', '/img/single.gif', '/img/ui.gif'):
                    if path == '/img/animsized' and query != 'width=400&height=200':
                        await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'wrong animation parameters: ' + query})
                    elif 'format' in query:
                        await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'an animation was asked for in a format: ' + query})
                    else:
                        body = {'/img/animsized': GIF, '/img/many.gif': MANY, '/img/single.gif': ONE, '/img/ui.gif': GIF}[path]
                        writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: %d\r\n\r\n' % len(body)).encode() + body)
                        await writer.drain()
                elif path == '/img/animated.gif':      # a proxy that answers with a GIF whatever was asked
                    writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: %d\r\n\r\n' % len(GIF)).encode() + GIF)
                    await writer.drain()
                elif path == '/img/photo.jpg':
                    if 'format=jpeg' not in query:
                        await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'no format=jpeg: ' + query})
                    else:
                        writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(JPG)).encode() + JPG)
                        await writer.drain()
                elif path == '/img/sized' and 'format=png&width=800&height=400' not in query:
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'wrong size parameters: ' + query})
                elif path == '/img/thumb' and 'format=png&width=320&height=213' not in query:
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'wrong thumbnail parameters: ' + query})
                elif path == '/img/thumb2' and 'format=png&width=150&height=100' not in query:
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'wrong thumbnail parameters: ' + query})
                elif 'format=png' not in query:
                    await reply(writer, HTTPStatus.BAD_REQUEST, {'message': 'no format=png: ' + query})
                else:
                    writer.write(('HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n' % len(PNG)).encode() + PNG)
                    await writer.drain()
                if headers.get('connection', '').lower() == 'close':
                    break
                continue
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
            elif method == 'GET' and path == '/api/v10/channels/900/messages':
                await reply(writer, HTTPStatus.OK, [])
            elif method in ('PUT', 'DELETE') and '/reactions/' in path and path.endswith('/@me'):
                import urllib.parse
                parts = path.split('/')                 # '', api, v10, channels, CID, messages, MID, reactions, EMOJI, @me
                cid, mid, emoji = parts[4], parts[6], urllib.parse.unquote(parts[8])
                state.setdefault('reacts', []).append((method, emoji))
                if emoji == '\U0001F608':
                    await reply(writer, HTTPStatus.FORBIDDEN, {'message': 'Missing Permissions', 'code': 50013})
                else:
                    writer.write(b'HTTP/1.1 204 No Content\r\n\r\n')
                    await writer.drain()
                    em = {'id': None, 'name': emoji}
                    if ':' in emoji:
                        em = {'id': emoji.split(':')[1], 'name': emoji.split(':')[0]}
                    await broadcast('MESSAGE_REACTION_ADD' if method == 'PUT' else 'MESSAGE_REACTION_REMOVE',
                                    {'user_id': '42', 'channel_id': cid, 'message_id': mid, 'emoji': em})
            elif method == 'POST' and path == '/api/v10/channels/200/typing':
                state['typing_posts'] = state.get('typing_posts', 0) + 1
                writer.write(b'HTTP/1.1 204 No Content\r\n\r\n')
                await writer.drain()
            elif method == 'POST' and path == '/api/v10/users/@me/channels':
                rid = json.loads(body)['recipient_id']
                if rid == '666':
                    await reply(writer, HTTPStatus.FORBIDDEN, {'message': 'Cannot send messages to this user', 'code': 50007})
                else:
                    await reply(writer, HTTPStatus.OK, {'id': '900', 'type': 1, 'recipients': [{'id': rid, 'username': 'someone'}]})
            elif method == 'POST' and path == '/api/v10/channels/900/messages':
                content = json.loads(body)['content']
                state['next_id'] += 1
                m = msg(state['next_id'], content, author='42', name='Kallisti-bot', channel='900', guild=None)
                await reply(writer, HTTPStatus.OK, m)
                await broadcast('MESSAGE_CREATE', m)
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
                    if content == 'pic':
                        state['next_id'] += 1
                        pm = msg(state['next_id'], 'a picture', author='7', name='someone')
                        pm['attachments'] = [{'id': '1', 'filename': 'cat.png', 'size': 1,
                                              'url': 'https://cdn.invalid/cat.png',
                                              'proxy_url': '%s://localhost:%d/img/cat.png?ex=1&hm=abc' % (SCHEME_HTTP, PORT),
                                              'width': 300, 'height': 200, 'content_type': 'image/png'}]
                        await broadcast('MESSAGE_CREATE', pm)
                    if content == 'typingposts':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'typing posts: %d' % state.get('typing_posts', 0), author='7', name='someone'))
                    if content == 'typing':
                        await broadcast('TYPING_START', {'channel_id': '200', 'guild_id': '100', 'user_id': '7', 'timestamp': 1,
                                                         'member': {'nick': 'Typist', 'user': {'id': '7', 'username': 'someone'}}})
                    if content == 'typingdone':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'done typing', author='7', name='Typist'))
                    if content == 'savefile':
                        state['next_id'] += 1
                        fm = msg(state['next_id'], 'a file', author='7', name='someone')
                        fm['attachments'] = [{'id': '2', 'filename': 'notes.bin', 'size': 70000,
                                              'url': '%s://localhost:%d/img/file.bin?sig=zz&format=png' % (SCHEME_HTTP, PORT),
                                              'proxy_url': 'https://media.invalid/never-used', 'content_type': 'application/octet-stream'}]
                        await broadcast('MESSAGE_CREATE', fm)
                    if content == 'uemoji':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'hi \U0001F600 \u2764\ufe0f \U0001F44D\U0001F3FD bye', author='7', name='someone'))
                    if content == 'emojiauthor':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'from a fancy name', author='9', name='\U0001F600 Bob'))
                    if content == 'newchan':
                        await broadcast('CHANNEL_CREATE', {'id': '230', 'guild_id': '100', 'name': '\U0001F3AE-games', 'type': 0, 'position': 9})
                    if content == 'emoji':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'cool <:blob:123> and <a:party:456> yes', author='7', name='someone'))
                    if content == 'gifpic':
                        state['next_id'] += 1
                        gm = msg(state['next_id'], 'a gif', author='7', name='someone')
                        gm['attachments'] = [{'id': '2', 'filename': 'dance.gif', 'size': 1, 'url': 'https://cdn.invalid/dance.gif',
                                              'proxy_url': '%s://localhost:%d/img/ui.gif?ex=1' % (SCHEME_HTTP, PORT),
                                              'width': 25, 'height': 15, 'content_type': 'image/gif'}]
                        await broadcast('MESSAGE_CREATE', gm)
                    if content == 'embed':
                        state['next_id'] += 1
                        em = msg(state['next_id'], 'a link', author='7', name='someone')
                        em['embeds'] = [{'type': 'article', 'title': 'Cool Page', 'description': 'about cats',
                                         'provider': {'name': 'Example'},
                                         'thumbnail': {'url': 'https://example.invalid/cat.png',
                                                       'proxy_url': '%s://localhost:%d/img/cat.png?ex=1' % (SCHEME_HTTP, PORT),
                                                       'width': 300, 'height': 200}}]
                        await broadcast('MESSAGE_CREATE', em)
                    if content == 'unfurl':           # the preview arrives later, as Discord does it
                        state['next_id'] += 1
                        um = msg(state['next_id'], 'later http://x', author='7', name='someone')
                        await broadcast('MESSAGE_CREATE', um)
                        await asyncio.sleep(0.2)
                        await broadcast('MESSAGE_UPDATE', {'id': str(state['next_id']), 'channel_id': '200', 'guild_id': '100',
                                                           'embeds': [{'type': 'link', 'title': 'Unfurled Title'}]})
                    if content == 'dmme':
                        state['next_id'] += 1
                        await broadcast('MESSAGE_CREATE', msg(state['next_id'], 'psst, a DM', author='8', name='eight', channel='901', guild=None))
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
                assert d['token'] == TOKEN and d['intents'] == (1 | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15)), d
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
