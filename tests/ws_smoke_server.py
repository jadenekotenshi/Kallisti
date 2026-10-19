#!/usr/bin/env python3
"""WebSocket server for ws_smoke, built on the independent `websockets` library (no compression).

usage: ws_smoke_server.py PORT [CERT KEY]    (with CERT/KEY: TLS 1.2, ECDHE-RSA-AES128-GCM-SHA256)
Routes: /echo (echoes every message), /greet (text, a 3-fragment message, a ping it waits to be
answered, then a text), /big (200 KB binary then a ~1 MB text), /close (a text, then close 4001),
anything else gets a 404 instead of an upgrade.
"""
import asyncio, ssl, sys
from http import HTTPStatus
from websockets.asyncio.server import serve

PORT = int(sys.argv[1])
CERT = sys.argv[2] if len(sys.argv) > 2 else None
KEY = sys.argv[3] if len(sys.argv) > 3 else None
BIG = bytes((i * 7 + 3) & 0xff for i in range(200 * 1024))
LONGTEXT = ('snowman ☃ and grin \U0001F600 ' * 40000)[:1000000]


def process_request(connection, request):
    if request.path not in ('/echo', '/greet', '/big', '/close'):
        return connection.respond(HTTPStatus.NOT_FOUND, 'no such route\n')
    return None


async def handler(ws):
    path = ws.request.path
    if path == '/echo':
        async for message in ws:
            await ws.send(message)
    elif path == '/greet':
        await ws.send('hello')
        await ws.send([b'part1 ', b'part2 ', b'part3'])      # one message, three frames
        pong = await ws.ping(b'are-you-there')
        await pong
        await ws.send('pong-ok')
        await ws.wait_closed()
    elif path == '/big':
        await ws.send(BIG)
        await ws.send(LONGTEXT)
        await ws.wait_closed()
    elif path == '/close':
        await ws.send('last')
        await ws.close(4001, 'server says bye')


async def main():
    ctx = None
    if CERT:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers('ECDHE-RSA-AES128-GCM-SHA256')
        ctx.load_cert_chain(CERT, KEY)
    async with serve(handler, '127.0.0.1', PORT, ssl=ctx, compression=None,
                     process_request=process_request, max_size=4 * 1024 * 1024):
        await asyncio.get_running_loop().create_future()

asyncio.run(main())
