import asyncio, websockets, time, sys
UP = "ws://localhost:38281"
def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)
async def handler(client):
    log("client connected", client.remote_address, dict(client.request.headers) if hasattr(client,'request') and client.request else "")
    try:
        async with websockets.connect(UP, max_size=None) as up:
            async def c2s():
                async for m in client:
                    log("C->S", str(m)[:400]); await up.send(m)
            async def s2c():
                async for m in up:
                    log("S->C", str(m)[:200]); await client.send(m)
            done, pending = await asyncio.wait([asyncio.create_task(c2s()), asyncio.create_task(s2c())], return_when=asyncio.FIRST_COMPLETED)
            for t in done:
                if t.exception(): log("EXC", repr(t.exception()))
    except Exception as e:
        log("handler error", repr(e))
    log("client closed", client.close_code, client.close_reason)
async def main():
    async with websockets.serve(handler, "localhost", 38282, max_size=None):
        log("proxy on 38282"); await asyncio.Future()
asyncio.run(main())
