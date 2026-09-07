import asyncio
import json
from contextlib import asynccontextmanager

import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed
from walkie_relay.server import create_server
from walkie_relay.state import Relay, Session, OUTBOX_LIMIT
from walkie_relay.protocol import encode

FRAME = bytes(range(256)) * 2 + bytes(128)


@pytest.fixture
async def relay():
    state = Relay()
    async with create_server(state, '127.0.0.1', 0) as server:
        yield state, f'ws://127.0.0.1:{server.sockets[0].getsockname()[1]}'


async def message(ws, kind, **fields):
    raw = await asyncio.wait_for(ws.recv(), 1)
    value = json.loads(raw)
    assert value['type'] == kind, value
    for key, expected in fields.items():
        assert value[key] == expected
    return value


@asynccontextmanager
async def registered(url, device='a', peer='b'):
    async with connect(url, compression=None, proxy=None) as ws:
        await ws.send(encode('hello', protocol=1, device_id=device, peer_id=peer))
        await message(ws, 'hello_ok', protocol=1)
        yield ws


@asynccontextmanager
async def pair(url):
    async with registered(url) as a:
        await message(a, 'peer_status', online=False)
        async with registered(url, 'b', 'a') as b:
            await message(b, 'peer_status', online=True)
            await message(a, 'peer_status', online=True)
            yield a, b


async def grant(a, b):
    await a.send(encode('ptt_start'))
    await message(a, 'floor_granted')
    await message(b, 'rx_start')


async def test_half_duplex_and_forwarding(relay):
    state, url = relay
    async with pair(url) as (a, b):
        assert len(state.sessions) == 2
        await grant(a, b)
        await a.send(FRAME)
        assert await asyncio.wait_for(b.recv(), 1) == FRAME
        await b.send(encode('ptt_start'))
        await message(b, 'floor_denied', reason='busy')
        await b.send(FRAME)
        await message(b, 'error', reason='not_floor_owner')
        await a.send(encode('ptt_stop'))
        await message(a, 'floor_released')  # would fail if B's audio reached A
        await message(b, 'rx_stop')
        await grant(b, a)
        await b.send(FRAME)
        assert await asyncio.wait_for(a.recv(), 1) == FRAME
        assert state.stats.floor_grants == 2
        assert state.stats.floor_denials == 1
        assert state.stats.frames_forwarded == 2


async def test_duplicate(relay):
    state, url = relay
    async with registered(url) as a:
        await message(a, 'peer_status', online=False)
        async with connect(url, proxy=None) as duplicate:
            await duplicate.send(encode('hello', protocol=1, device_id='a', peer_id='b'))
            await message(duplicate, 'error', reason='duplicate_device')
            with pytest.raises(ConnectionClosed):
                await duplicate.recv()
        assert 'a' in state.sessions
        await a.send(encode('ptt_start'))
        await message(a, 'floor_denied', reason='peer_offline')


@pytest.mark.parametrize('owner_disconnect', [True, False])
async def test_disconnect_releases_and_can_rejoin(relay, owner_disconnect):
    state, url = relay
    async with pair(url) as (a, b):
        await grant(a, b)
        departing, staying = (a, b) if owner_disconnect else (b, a)
        await departing.close()
        await message(staying, 'rx_stop' if owner_disconnect else 'floor_released')
        await message(staying, 'peer_status', online=False)
        assert not state.floors
        await staying.send(encode('ptt_start'))
        await message(staying, 'floor_denied', reason='peer_offline')
        device, peer = ('a', 'b') if owner_disconnect else ('b', 'a')
        async with registered(url, device, peer) as rejoined:
            await message(rejoined, 'peer_status', online=True)
            await message(staying, 'peer_status', online=True)
            await grant(staying, rejoined)


@pytest.mark.parametrize('raw', ['{', '[]', 'null', '42', '{"type":1}', '{"type":"surprise"}'])
async def test_malformed_controls_server_survives(relay, raw):
    _, url = relay
    async with pair(url) as (a, b):
        await a.send(raw)
        await message(a, 'error')
        await grant(a, b)


@pytest.mark.parametrize('size', [0, 1, 639, 641, 4096])
async def test_wrong_audio_size_not_forwarded(relay, size):
    state, url = relay
    async with pair(url) as (a, b):
        await grant(a, b)
        await a.send(bytes(size))
        await message(a, 'error', reason='invalid_audio_size')
        await a.send(FRAME)
        assert await asyncio.wait_for(b.recv(), 1) == FRAME
        assert state.stats.invalid_frames == 1


@pytest.mark.parametrize('raw', ['[]', '{', b'audio', encode('hello', protocol=True, device_id='a', peer_id='b'),
                                     encode('hello', protocol=2, device_id='a', peer_id='b')])
async def test_bad_handshake(relay, raw):
    _, url = relay
    async with connect(url, proxy=None) as ws:
        await ws.send(raw)
        await message(ws, 'error', reason='invalid_hello')
        with pytest.raises(ConnectionClosed):
            await ws.recv()


async def test_nonreciprocal_pair_is_offline(relay):
    _, url = relay
    async with registered(url) as a, registered(url, 'b', 'c') as b:
        await message(a, 'peer_status', online=False)
        await message(b, 'peer_status', online=False)
        await a.send(encode('ptt_start'))
        await message(a, 'floor_denied', reason='peer_offline')


async def test_simultaneous_requests(relay):
    state, url = relay
    async with pair(url) as (a, b):
        await asyncio.gather(a.send(encode('ptt_start')), b.send(encode('ptt_start')))
        replies = [[json.loads(await asyncio.wait_for(ws.recv(), 1))] for ws in (a, b)]
        winner = next(i for i, r in enumerate(replies) if r[0]['type'] == 'floor_granted')
        loser = 1 - winner
        replies[loser].append(json.loads(await asyncio.wait_for((a, b)[loser].recv(), 1)))
        assert {m['type'] for m in replies[loser]} == {'rx_start', 'floor_denied'}
        assert len(state.floors) == 1


def test_bounded_outbox_preserves_controls():
    s = Session(None)
    s.control('rx_start')
    for _ in range(100):
        s.emit(FRAME)
    assert len(s.outbox) == OUTBOX_LIMIT
    assert isinstance(s.outbox[0], str)
    assert s.drops > 0
    s.clear_audio()
    s.control('rx_stop')
    assert [json.loads(m)['type'] for m in s.outbox] == ['rx_start', 'rx_stop']
    for _ in range(100):
        s.control('error')
    assert s.failed
    assert len(s.outbox) == OUTBOX_LIMIT
