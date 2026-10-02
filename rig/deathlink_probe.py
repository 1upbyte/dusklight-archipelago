"""Stand-in for "another player" when testing death link with only one copy of the game.

Joins a room as a second slot with the DeathLink tag, prints every death the game sends,
and sends one to the game when you press Enter.

    python rig/deathlink_probe.py <slot name> [server] [password]

The slot has to exist in the room: generate a two-player seed (your YAML plus a second one
named after this slot) and host it with MultiServer.py.
"""
import asyncio
import json
import sys
import time
import uuid

import websockets

GAME = "Twilight Princess (Dusklight)"


def log(*parts):
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


async def main(slot: str, server: str, password: str) -> None:
    url = server if server.startswith(("ws://", "wss://")) else f"ws://{server}"
    async with websockets.connect(url, max_size=None) as ws:
        await ws.recv()  # RoomInfo
        await ws.send(json.dumps([{
            "cmd": "Connect", "game": GAME, "name": slot, "password": password,
            "uuid": uuid.uuid4().hex, "items_handling": 0, "tags": ["DeathLink"],
            "version": {"major": 0, "minor": 6, "build": 2, "class": "Version"},
            "slot_data": False,
        }]))
        reply = json.loads(await ws.recv())
        if reply[0]["cmd"] != "Connected":
            log("refused:", reply[0].get("errors"))
            return
        log(f"connected as {slot}. Press Enter to kill the other players, Ctrl+C to quit.")

        async def listen():
            async for raw in ws:
                for packet in json.loads(raw):
                    if packet.get("cmd") == "Bounced" and "DeathLink" in packet.get("tags", []):
                        data = packet.get("data", {})
                        if data.get("source") != slot:
                            log("death received:", data.get("cause") or data.get("source"))

        async def send_on_enter():
            loop = asyncio.get_running_loop()
            while True:
                if not await loop.run_in_executor(None, sys.stdin.readline):
                    await asyncio.Future()  # stdin closed: keep listening, stop sending
                await ws.send(json.dumps([{
                    "cmd": "Bounce", "tags": ["DeathLink"],
                    "data": {"time": time.time(), "source": slot,
                             "cause": f"{slot} tripped over the probe."},
                }]))
                log("death sent")

        await asyncio.gather(listen(), send_on_enter())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    try:
        asyncio.run(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "localhost:38281",
                         sys.argv[3] if len(sys.argv) > 3 else ""))
    except KeyboardInterrupt:
        pass
