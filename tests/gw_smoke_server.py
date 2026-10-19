#!/usr/bin/env python3
"""A scripted fake Discord gateway for gw_smoke, built on the independent `websockets` library.

usage: gw_smoke_server.py PORT REPORT [CERT KEY]
Four connections, each checking what the client sends:
  1  HELLO(1000ms); IDENTIFY validated; READY(sess-1) + GUILD_CREATE + MESSAGE_CREATE; two heartbeats ACKed; op 7
  2  RESUME(sess-1, seq 3); MESSAGE_CREATE s=4, RESUMED; op 9 (not resumable); IDENTIFY again; READY(sess-2); close 4000
  3  RESUME(sess-2); RESUMED; heartbeats are never ACKed (a zombie), the client must drop us
  4  RESUME(sess-2); RESUMED; close 4004 (the client must give up)
REPORT receives a line per problem, then "DONE" once the script ran to the end.
"""
import asyncio, json, ssl, sys
from websockets.asyncio.server import serve

PORT = int(sys.argv[1])
REPORT = sys.argv[2]
CERT = sys.argv[3] if len(sys.argv) > 3 else None
KEY = sys.argv[4] if len(sys.argv) > 4 else None
TOKEN = 'Bot test.token.value'
INTENTS = (1 << 0) | (1 << 9) | (1 << 15)
problems = []
nconn = 0
done = None


def check(cond, what):
    if not cond:
        problems.append(what)


def frame(op, d=None, s=None, t=None):
    return json.dumps({'op': op, 'd': d, 's': s, 't': t})


def ready(sess):
    return frame(0, {'session_id': sess, 'resume_gateway_url': 'ws%s://localhost:%d' % ('s' if CERT else '', PORT),
                     'user': {'id': '42', 'username': 'Kallisti-bot'}, 'guilds': [{'id': '100', 'unavailable': True}]},
                 1, 'READY')


async def recv(ws, timeout=8):
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def next_nonhb(ws, state):
    """Reads frames, ACKing heartbeats, until something else arrives."""
    while True:
        m = await recv(ws)
        if m['op'] == 1:
            state['hb'] += 1
            await ws.send(frame(11))
        else:
            return m


async def handler(ws):
    global nconn
    nconn += 1
    n = nconn
    state = {'hb': 0}
    await ws.send(frame(10, {'heartbeat_interval': 1000}))
    if n == 1:
        m = await next_nonhb(ws, state)
        check(m['op'] == 2, 'conn1: expected IDENTIFY, got op %s' % m['op'])
        d = m.get('d', {})
        check(d.get('token') == TOKEN, 'conn1: bad token')
        check(d.get('intents') == INTENTS, 'conn1: bad intents %r' % d.get('intents'))
        check('os' in d.get('properties', {}), 'conn1: no properties')
        await ws.send(ready('sess-1'))
        await ws.send(frame(0, {'id': '100', 'name': 'Test Guild', 'channels': [
            {'id': '200', 'name': 'general', 'type': 0}, {'id': '201', 'name': 'voice', 'type': 2}]}, 2, 'GUILD_CREATE'))
        await ws.send(frame(0, {'id': '300', 'channel_id': '200', 'content': 'hello \u2603',
                                'author': {'id': '7', 'username': 'someone'}}, 3, 'MESSAGE_CREATE'))
        while state['hb'] < 3:
            m = await recv(ws)
            if m['op'] == 1:
                state['hb'] += 1
                await ws.send(frame(11))
            else:
                check(False, 'conn1: unexpected op %s' % m['op'])
        await ws.send(frame(7))
        await ws.wait_closed()
    elif n == 2:
        m = await next_nonhb(ws, state)
        check(m['op'] == 6, 'conn2: expected RESUME, got op %s' % m['op'])
        d = m.get('d', {})
        check(d.get('session_id') == 'sess-1' and d.get('seq') == 3 and d.get('token') == TOKEN,
              'conn2: bad RESUME %r' % d)
        await ws.send(frame(0, {'id': '301', 'channel_id': '200', 'content': 'replayed',
                                'author': {'id': '7', 'username': 'someone'}}, 4, 'MESSAGE_CREATE'))
        await ws.send(frame(0, {}, 5, 'RESUMED'))
        await ws.send(frame(9, False))
        m = await next_nonhb(ws, state)
        check(m['op'] == 2, 'conn2: expected IDENTIFY after INVALID_SESSION, got op %s' % m['op'])
        await ws.send(ready('sess-2'))
        await ws.close(4000, 'unknown error')
    elif n == 3:
        m = await next_nonhb(ws, state)
        check(m['op'] == 6 and m['d'].get('session_id') == 'sess-2' and m['d'].get('seq') == 1,
              'conn3: bad RESUME %r' % m)
        await ws.send(frame(0, {}, 2, 'RESUMED'))
        # never ACK: the client must notice and drop the connection by itself
        try:
            while True:
                await recv(ws, 6)
        except asyncio.TimeoutError:
            check(False, 'conn3: the client never dropped a connection that stopped ACKing')
        except Exception:
            pass
    elif n == 4:
        m = await next_nonhb(ws, state)
        check(m['op'] == 6 and m['d'].get('session_id') == 'sess-2', 'conn4: bad RESUME %r' % m)
        await ws.send(frame(0, {}, 3, 'RESUMED'))
        await ws.close(4004, 'Authentication failed.')
        await asyncio.sleep(2)           # a client that reconnects would show up as a fifth connection
        check(nconn == 4, 'conn4: the client reconnected after a fatal close')
        done.set()


async def main():
    global done
    done = asyncio.Event()
    ctx = None
    if CERT:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers('ECDHE-RSA-AES128-GCM-SHA256')
        ctx.load_cert_chain(CERT, KEY)
    async with serve(handler, '127.0.0.1', PORT, ssl=ctx, compression=None):
        await done.wait()
        with open(REPORT, 'w') as f:
            for p in problems:
                f.write('FAIL ' + p + '\n')
            f.write('DONE\n')
        await asyncio.sleep(0.5)

asyncio.run(main())
