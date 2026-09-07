import argparse
import asyncio
import logging
from urllib.parse import urlsplit
from .audio import Audio
from .client import Client
from .protocol import valid_id


def main() -> None:
    parser = argparse.ArgumentParser(description="Laptop PCM walkie-talkie; SPACE toggles PTT, Q quits")
    parser.add_argument("--server")
    parser.add_argument("--device")
    parser.add_argument("--peer")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--input-device", type=int)
    parser.add_argument("--output-device", type=int)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.debug else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    # Keep library packet dumps out of the useful application statistics.
    logging.getLogger("websockets").setLevel(logging.WARNING)
    try:
        if args.list_devices:
            import sounddevice as sd
            try:
                print(sd.query_devices())
            except sd.PortAudioError as exc:
                raise RuntimeError(f"Cannot enumerate audio devices: {exc}") from exc
            print("Default input/output indexes:", sd.default.device)
            return
        if not valid_id(args.device) or not valid_id(args.peer) or args.device == args.peer:
            parser.error("--device and --peer must be different IDs (1–64 ASCII letters/digits/-_.)")
        if not args.server or urlsplit(args.server).scheme not in ("ws", "wss") or not urlsplit(args.server).hostname:
            parser.error("--server must be a ws:// or wss:// URL")
        asyncio.run(Client(args.server, args.device, args.peer,
                           Audio(args.input_device, args.output_device)).run())
    except KeyboardInterrupt:
        pass
    except (RuntimeError, OSError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
