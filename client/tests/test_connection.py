"""Client network tests use a tiny wire peer, not imports from the relay."""
import asyncio
import json
import os
import sys
import termios
from contextlib import suppress

import pytest
from websockets.asyncio.server import serve

from walkie_client.audio import Audio
from walkie_client.client import Client
from walkie_client.protocol import encode
from walkie_client.ptt import TerminalPTT


async def eventually(predicate):
    async with asyncio.timeout(3):
        while not predicate():
            await asyncio.sleep(0.005)


async def test_real_socket_tx_and_reconnect_resets_floor():
    received = []
    connections = 0
    reconnected = asyncio.Event()
    finish = asyncio.Event()
    audio = Audio()
    client = None

    async def handler(ws):
        nonlocal connections
        connections += 1
        received.append(json.loads(await ws.recv()))
        await ws.send(encode('hello_ok', protocol=1))
        await ws.send(encode('peer_status', online=True))
        if connections == 1:
            received.append(json.loads(await ws.recv()))
            await ws.send(encode('floor_granted'))
            received.append(await ws.recv())
            await ws.close()
        else:
            reconnected.set()
            await finish.wait()

    async with serve(handler, '127.0.0.1', 0, compression=None) as server:
        url = f'ws://127.0.0.1:{server.sockets[0].getsockname()[1]}'
        client = Client(url, 'a', 'b', audio)
        task = asyncio.create_task(client.connections())
        try:
            await eventually(lambda: client.online)
            client.toggle()
            assert not audio.enabled
            await eventually(lambda: audio.enabled)
            from types import SimpleNamespace
            frame = b'\x01\x02' * 320
            audio.input_callback(frame, 320, None, SimpleNamespace(input_overflow=False))
            await asyncio.wait_for(reconnected.wait(), 3)
            await eventually(lambda: client.ws is not None)
            assert not audio.enabled
            assert client.phase == 'idle'
            assert not audio.tx and not audio.jitter.frames
            assert received[0] == received[3] == {
                'type': 'hello', 'protocol': 1, 'device_id': 'a', 'peer_id': 'b'}
            assert received[1] == {'type': 'ptt_start'}
            assert received[2] == frame
        finally:
            finish.set()
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task


async def test_terminal_restored_after_exception(monkeypatch):
    master, slave = os.openpty()
    try:
        with os.fdopen(slave, 'r', closefd=False) as stream:
            monkeypatch.setattr(sys, 'stdin', stream)
            before = termios.tcgetattr(slave)
            calls = []
            with pytest.raises(RuntimeError, match='test failure'):
                with TerminalPTT(lambda: calls.append('toggle'), lambda: calls.append('quit')):
                    assert not termios.tcgetattr(slave)[3] & termios.ICANON
                    os.write(master, b' q')
                    await eventually(lambda: len(calls) == 2)
                    raise RuntimeError('test failure')
            assert calls == ['toggle', 'quit']
            assert termios.tcgetattr(slave) == before
    finally:
        os.close(master)
        os.close(slave)
