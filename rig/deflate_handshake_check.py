"""Does an Archipelago-configured server accept the mod's WebSocket upgrade with compression?

Sends the mod's exact upgrade request (src/ap/ws_tcp.cpp) over a raw socket to a websockets
server configured like MultiServer, and runs MultiServer's own check (the one that sends
"your client does not support compressed websocket connections") on the connection.

    python rig/deflate_handshake_check.py
"""
import asyncio
import base64
import os
import socket

import websockets
from websockets.extensions.permessage_deflate import PerMessageDeflate, ServerPerMessageDeflateFactory

# As MultiServer.py configures it.
FACTORY = ServerPerMessageDeflateFactory(server_max_window_bits=11, client_max_window_bits=11,
                                         compress_settings={"memLevel": 4})

# As TcpWebSocket::send_upgrade writes it.
REQUEST = ("GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Extensions: permessage-deflate\r\n"
           "User-Agent: Dusklight-Archipelago\r\n\r\n")

verdict = {}


async def handler(ws):
    # MultiServer.py: `if not any(isinstance(extension, PerMessageDeflate) for extension in
    # client.socket.extensions): notify "... does not support compressed ..."`
    verdict["negotiated"] = any(isinstance(e, PerMessageDeflate) for e in ws.extensions)
    await ws.send('[{"cmd": "RoomInfo", "note": "' + "compress me " * 40 + '"}]')
    await asyncio.sleep(0.5)


def client(port: int) -> tuple[str, int]:
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(REQUEST.format(host=f"127.0.0.1:{port}", key=key).encode())
    data = b""
    while b"\r\n\r\n" not in data:
        data += s.recv(4096)
    head, _, rest = data.partition(b"\r\n\r\n")
    while len(rest) < 2:
        rest += s.recv(4096)
    s.close()
    ext = [l for l in head.decode().split("\r\n") if l.lower().startswith("sec-websocket-extensions")]
    return (ext[0] if ext else "(none)"), rest[0]


async def main() -> int:
    async with websockets.serve(handler, "127.0.0.1", 0, extensions=[FACTORY]) as server:
        port = server.sockets[0].getsockname()[1]
        header, first_byte = await asyncio.to_thread(client, port)
    await asyncio.sleep(0.1)
    print("server response header :", header)
    print("MultiServer's check    :", "passes (no warning)" if verdict.get("negotiated") else "FAILS (warning)")
    print("first frame compressed :", bool(first_byte & 0x40), f"(byte 0x{first_byte:02x})")
    return 0 if verdict.get("negotiated") and first_byte & 0x40 else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
