"""All routing/floor mutations run synchronously on the asyncio thread."""
import asyncio
import logging
from collections import deque
from dataclasses import dataclass, field
from websockets.asyncio.server import ServerConnection
from .protocol import FRAME_BYTES, VERSION, encode, valid_id

log = logging.getLogger(__name__)
OUTBOX_LIMIT = 16


@dataclass(eq=False)
class Session:
    ws: ServerConnection
    device: str = ""
    peer: str = ""
    outbox: deque[str | bytes] = field(default_factory=deque)
    ready: asyncio.Event = field(default_factory=asyncio.Event)
    failed: bool = False
    drops: int = 0

    def emit(self, message: str | bytes) -> None:
        if self.failed:
            return
        if len(self.outbox) >= OUTBOX_LIMIT:
            # Preserve control ordering. Evict old audio first, never controls.
            for i, old in enumerate(self.outbox):
                if isinstance(old, bytes):
                    del self.outbox[i]
                    self.drops += 1
                    break
            else:
                self.failed = True
                self.ready.set()
                return
        self.outbox.append(message)
        self.ready.set()

    def control(self, kind: str, **fields: object) -> None:
        self.emit(encode(kind, **fields))

    def clear_audio(self) -> None:
        self.outbox = deque(m for m in self.outbox if isinstance(m, str))


@dataclass
class Stats:
    frames_forwarded: int = 0
    invalid_frames: int = 0
    floor_grants: int = 0
    floor_denials: int = 0
    queue_drops: int = 0


class Relay:
    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.floors: dict[tuple[str, str], str] = {}
        self.stats = Stats()

    def pair(self, s: Session) -> tuple[str, str]:
        return tuple(sorted((s.device, s.peer)))

    def peer(self, s: Session) -> Session | None:
        peer = self.sessions.get(s.peer)
        return peer if peer and peer.peer == s.device else None

    def error(self, s: Session, reason: str) -> None:
        log.warning("protocol error device=%s reason=%s", s.device or "unregistered", reason)
        s.control("error", reason=reason)

    def register(self, s: Session, message: dict) -> bool:
        device, peer = message.get("device_id"), message.get("peer_id")
        if (message.get("type") != "hello" or type(message.get("protocol")) is not int
                or message["protocol"] != VERSION or not valid_id(device)
                or not valid_id(peer) or device == peer):
            self.error(s, "invalid_hello")
            return False
        if device in self.sessions:
            self.error(s, "duplicate_device")
            return False
        s.device, s.peer = device, peer
        self.sessions[device] = s
        s.control("hello_ok", protocol=VERSION)
        p = self.peer(s)
        s.control("peer_status", online=p is not None)
        log.info("registered device=%s peer=%s online=%s", device, peer, p is not None)
        if p:
            p.control("peer_status", online=True)
            log.info("peer online %s <-> %s", device, peer)
        return True

    def release(self, s: Session, reason: str) -> None:
        key = self.pair(s)
        owner_id = self.floors.pop(key, None)
        if owner_id is None:
            return
        owner = self.sessions.get(owner_id)
        receiver = self.sessions.get(key[0] if key[1] == owner_id else key[1])
        if receiver:
            receiver.clear_audio()
            receiver.control("rx_stop", reason=reason)
        if owner:
            owner.control("floor_released", reason=reason)
        log.info("floor release owner=%s reason=%s", owner_id, reason)

    def control(self, s: Session, message: dict) -> None:
        kind = message["type"]
        key = self.pair(s)
        if kind == "ptt_start":
            reason = "peer_offline" if not self.peer(s) else "busy" if key in self.floors else None
            if reason:
                self.stats.floor_denials += 1
                s.control("floor_denied", reason=reason)
                log.info("floor denied device=%s reason=%s", s.device, reason)
            else:
                self.floors[key] = s.device
                self.stats.floor_grants += 1
                s.control("floor_granted")
                self.peer(s).control("rx_start", device_id=s.device)
                log.info("floor granted device=%s", s.device)
        elif kind == "ptt_stop":
            if self.floors.get(key) == s.device:
                self.release(s, "ptt_stop")
            else:
                s.control("floor_released", reason="not_owner")
        else:
            self.error(s, "unknown_control")

    def audio(self, s: Session, frame: bytes) -> None:
        if len(frame) != FRAME_BYTES:
            self.stats.invalid_frames += 1
            self.error(s, "invalid_audio_size")
        elif self.floors.get(self.pair(s)) != s.device:
            self.stats.invalid_frames += 1
            self.error(s, "not_floor_owner")
        elif (p := self.peer(s)) is not None:
            p.emit(frame)
            self.stats.frames_forwarded += 1
            if self.stats.frames_forwarded % 500 == 0:
                log.debug("stats=%s outgoing_depth=%d", self.stats, len(p.outbox))

    def disconnect(self, s: Session) -> None:
        if self.sessions.get(s.device) is not s:
            return
        self.release(s, "peer_disconnected")
        p = self.peer(s)
        del self.sessions[s.device]
        self.stats.queue_drops += s.drops
        if p:
            p.control("peer_status", online=False)
            log.info("peer offline device=%s peer=%s", s.device, p.device)
        log.info("disconnected device=%s", s.device)
