import asyncio
import logging
from contextlib import suppress
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed
from .protocol import MAX_MESSAGE, decode
from .state import Relay, Session

log = logging.getLogger(__name__)


async def writer(s: Session) -> None:
    try:
        while not s.failed:
            await s.ready.wait()
            while s.outbox and not s.failed:
                message = s.outbox.popleft()
                # TCP cannot retract sent audio; disconnect a stalled consumer.
                async with asyncio.timeout(0.5):
                    await s.ws.send(message)
            s.ready.clear()
    except (TimeoutError, ConnectionClosed):
        pass
    finally:
        await s.ws.close(code=1011, reason="writer stopped or slow consumer")


async def handle(ws: ServerConnection, relay: Relay) -> None:
    s = Session(ws)
    task = None
    log.info("client connect remote=%s", ws.remote_address)
    try:
        async with asyncio.timeout(5):
            raw = await ws.recv()
        try:
            message = decode(raw) if isinstance(raw, str) else {}
        except ValueError:
            message = {}
        if not relay.register(s, message):
            async with asyncio.timeout(0.5):
                await ws.send(s.outbox.popleft())
            await ws.close(code=1008, reason="registration rejected")
            return
        task = asyncio.create_task(writer(s))
        async for raw in ws:
            if isinstance(raw, bytes):
                relay.audio(s, raw)
            else:
                try:
                    relay.control(s, decode(raw))
                except ValueError as exc:
                    relay.error(s, str(exc))
    except (ConnectionClosed, TimeoutError):
        pass
    finally:
        relay.disconnect(s)
        if task:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task


def create_server(relay: Relay, host: str, port: int) -> serve:
    return serve(lambda ws: handle(ws, relay), host, port, compression=None,
                 max_size=MAX_MESSAGE, max_queue=8, write_limit=4096,
                 ping_interval=20, ping_timeout=20, close_timeout=1)


async def run(host: str, port: int) -> None:
    relay = Relay()
    try:
        async with create_server(relay, host, port):
            log.info("relay listening on %s:%d", host, port)
            await asyncio.Future()
    finally:
        log.info("final stats: %s", relay.stats)
