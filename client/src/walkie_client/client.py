import asyncio
import logging
from contextlib import suppress
from websockets.asyncio.client import connect, ClientConnection
from websockets.exceptions import ConnectionClosed, WebSocketException
from .audio import Audio
from .protocol import FRAME_BYTES, MAX_MESSAGE, VERSION, decode, encode
from .ptt import TerminalPTT

log = logging.getLogger(__name__)


class Client:
    def __init__(self, server: str, device: str, peer: str, audio: Audio) -> None:
        self.server, self.device, self.peer, self.audio = server, device, peer, audio
        self.ws: ClientConnection | None = None
        self.phase = "idle"
        self.online = False
        self.quit = asyncio.Event()
        self.controls: asyncio.Queue[str] = asyncio.Queue(maxsize=4)
        self.tx_frames = self.rx_frames = 0

    def reset(self) -> None:
        self.ws = None
        self.phase = "idle"
        self.online = False
        self.audio.set_transmitting(False)
        self.audio.jitter.reset()
        while not self.controls.empty():
            self.controls.get_nowait()

    def toggle(self) -> None:
        if self.ws is None:
            log.info("DISCONNECTED / RECONNECTING")
        elif self.phase in ("requested", "transmitting"):
            # Disable and flush immediately, before any network await.
            self.audio.set_transmitting(False)
            self.phase = "stopping"
            self.controls.put_nowait("ptt_stop")
            log.info("STOPPED")
        elif self.phase == "idle":
            self.phase = "requested"
            self.controls.put_nowait("ptt_start")
            log.info("REQUESTING FLOOR")
        # Ignore toggles until release is acknowledged: avoids stale grants.

    async def send(self, ws: ClientConnection) -> None:
        while True:
            if not self.controls.empty():
                payload = encode(self.controls.get_nowait())
            elif self.phase == "transmitting" and (frame := self.audio.take()) is not None:
                payload = frame
            else:
                await asyncio.sleep(0.005)
                continue
            # Single writer preserves controls/audio ordering. No await between
            # checking TX state and entering send. Already sent bytes can't retract.
            async with asyncio.timeout(0.5):
                await ws.send(payload)
            if isinstance(payload, bytes):
                self.tx_frames += 1

    async def receive(self, ws: ClientConnection) -> None:
        async for raw in ws:
            if isinstance(raw, bytes):
                if len(raw) != FRAME_BYTES:
                    raise ValueError("relay sent wrong audio frame size")
                self.rx_frames += 1
                self.audio.jitter.push(raw)
                if self.rx_frames % 500 == 0:
                    log.debug("rx=%d drops=%d underruns=%d", self.rx_frames,
                              self.audio.jitter.drops, self.audio.jitter.underruns)
                continue
            m = decode(raw)
            kind = m["type"]
            if kind == "peer_status":
                self.online = m.get("online") is True
                log.info("PEER %s", "ONLINE" if self.online else "OFFLINE")
                if not self.online:
                    self.audio.set_transmitting(False)
                    self.audio.jitter.reset()
            elif kind == "floor_granted":
                if self.phase == "requested":
                    self.phase = "transmitting"
                    self.audio.set_transmitting(True)
                    log.info("TRANSMITTING")
            elif kind == "floor_denied":
                if self.phase != "stopping":
                    self.phase = "idle"
                self.audio.set_transmitting(False)
                log.info("%s", "BUSY" if m.get("reason") == "busy" else "PEER OFFLINE")
            elif kind == "floor_released":
                self.phase = "idle"
                self.audio.set_transmitting(False)
                log.info("STOPPED (%s)", m.get("reason"))
            elif kind == "rx_start":
                self.audio.jitter.reset(active=True)
                log.info("RECEIVING")
            elif kind == "rx_stop":
                self.audio.jitter.reset()
                log.info("RECEIVE STOPPED (%s)", m.get("reason"))
            elif kind == "error":
                raise ValueError(f"relay error: {m.get('reason')}")
            else:
                raise ValueError(f"unexpected relay control: {kind}")

    async def session(self, ws: ClientConnection) -> None:
        tasks = [asyncio.create_task(self.receive(ws)), asyncio.create_task(self.send(ws))]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            self.audio.set_transmitting(False)
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if self.phase in ("transmitting", "requested", "stopping"):
                with suppress(ConnectionClosed, TimeoutError):
                    async with asyncio.timeout(0.5):
                        await ws.send(encode("ptt_stop"))

    async def connections(self) -> None:
        delay = 0.5
        while True:
            self.reset()
            started = asyncio.get_running_loop().time()
            try:
                async with connect(self.server, compression=None, max_size=MAX_MESSAGE,
                                   max_queue=8, write_limit=4096, open_timeout=5,
                                   ping_interval=20, ping_timeout=20, close_timeout=1,
                                   proxy=None) as ws:
                    await ws.send(encode("hello", protocol=VERSION,
                                         device_id=self.device, peer_id=self.peer))
                    async with asyncio.timeout(5):
                        raw = await ws.recv()
                    reply = decode(raw) if isinstance(raw, str) else {}
                    if reply.get("type") != "hello_ok" or reply.get("protocol") != VERSION:
                        raise ValueError(f"registration rejected: {reply}")
                    self.ws = ws
                    log.info("CONNECTED — SPACE toggles PTT; Q quits")
                    await self.session(ws)
            except (OSError, WebSocketException, TimeoutError, ValueError) as exc:
                log.warning("DISCONNECTED: %s", exc)
            finally:
                self.reset()
            if asyncio.get_running_loop().time() - started >= 10:
                delay = 0.5
            log.info("RECONNECTING in %.1fs", delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, 8)

    async def run(self) -> None:
        task = None
        try:
            with self.audio, TerminalPTT(self.toggle, self.quit.set):
                task = asyncio.create_task(self.connections())
                quit_task = asyncio.create_task(self.quit.wait())
                try:
                    done, _ = await asyncio.wait((task, quit_task), return_when=asyncio.FIRST_COMPLETED)
                    if task in done:
                        task.result()
                finally:
                    task.cancel()
                    quit_task.cancel()
                    await asyncio.gather(task, quit_task, return_exceptions=True)
        finally:
            log.info("final stats: TX=%d RX=%d RX_drops=%d playback_underruns=%d audio=%s",
                     self.tx_frames, self.rx_frames, self.audio.jitter.drops,
                     self.audio.jitter.underruns, self.audio.stats)
