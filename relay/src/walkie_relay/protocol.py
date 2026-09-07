"""Wire constants; PROTOCOL.md is authoritative, not the other package."""
import json

VERSION = 1
SAMPLE_RATE = 16_000
SAMPLES_PER_FRAME = 320
FRAME_BYTES = 640
MAX_MESSAGE = 4096


def encode(kind: str, **fields: object) -> str:
    return json.dumps({"type": kind, **fields}, separators=(",", ":"))


def decode(raw: str) -> dict:
    try:
        message = json.loads(raw)
    except (ValueError, RecursionError) as exc:
        raise ValueError("invalid_json") from exc
    if not isinstance(message, dict) or not isinstance(message.get("type"), str):
        raise ValueError("invalid_control")
    return message


def valid_id(value: object) -> bool:
    return (isinstance(value, str) and 1 <= len(value) <= 64
            and all(c.isascii() and (c.isalnum() or c in "-_.") for c in value))
