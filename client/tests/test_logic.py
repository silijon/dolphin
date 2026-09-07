from types import SimpleNamespace

import pytest
from walkie_client.audio import Audio, TX_MAX_FRAMES
from walkie_client.client import Client
from walkie_client.jitter import JitterBuffer, SILENCE
from walkie_client.protocol import encode

FRAME = b'\x01\x02' * 320
STATUS = SimpleNamespace(input_overflow=False, output_underflow=False)


def test_jitter_prebuffer_underflow_and_reset():
    j = JitterBuffer()
    j.push(FRAME)
    assert j.pop() == SILENCE
    j.reset(True)
    for _ in range(2):
        j.push(FRAME)
        assert j.pop() == SILENCE
    j.push(FRAME)
    assert [j.pop() for _ in range(3)] == [FRAME] * 3
    assert j.pop() == SILENCE
    assert j.underruns == 1
    j.push(FRAME)
    assert j.pop() == SILENCE
    j.reset()
    assert j.pop() == SILENCE
    assert not j.frames


def test_jitter_drops_oldest_and_never_waits():
    j = JitterBuffer(target=1, maximum=3)
    j.reset(True)
    for n in range(5):
        j.push(bytes([n]) * 640)
    assert j.drops == 2
    assert j.pop() == bytes([2]) * 640
    with j.lock:
        assert j.pop() == SILENCE
    with pytest.raises(ValueError):
        j.push(b'bad')


def test_capture_bounded_gated_and_flushed():
    audio = Audio()
    audio.input_callback(FRAME, 320, None, STATUS)
    assert audio.take() is None
    audio.set_transmitting(True)
    for _ in range(100):
        audio.input_callback(FRAME, 320, None, STATUS)
    assert len(audio.tx) == TX_MAX_FRAMES
    assert audio.stats.tx_drops == 100 - TX_MAX_FRAMES
    assert audio.take() == FRAME
    audio.set_transmitting(False)
    assert audio.take() is None
    audio.set_transmitting(True)
    audio.tx.append((audio.generation, 0, FRAME))
    assert audio.take() is None
    with audio.lock:
        audio.input_callback(FRAME, 320, None, STATUS)


class Messages:
    def __init__(self, *messages):
        self.messages = messages

    async def __aiter__(self):
        for m in self.messages:
            yield encode(m)


async def test_release_before_grant_never_enables_microphone():
    audio = Audio()
    c = Client('ws://localhost', 'a', 'b', audio)
    c.ws = object()
    c.toggle()
    assert c.phase == 'requested' and not audio.enabled
    c.toggle()
    assert c.phase == 'stopping'
    await c.receive(Messages('floor_granted'))
    assert not audio.enabled
    await c.receive(Messages('floor_released'))
    assert c.phase == 'idle'
    c.toggle()
    await c.receive(Messages('floor_granted'))
    assert audio.enabled
    c.reset()
    assert not audio.enabled and c.ws is None and c.controls.empty()


async def test_client_receive_disconnect_resets():
    # Real network integration lives in relay tests; state needs no PortAudio.
    c = Client('ws://localhost', 'a', 'b', Audio())
    await c.receive(Messages('rx_start'))
    assert c.audio.jitter.active
    c.audio.jitter.push(FRAME)
    c.reset()
    assert not c.audio.jitter.active and not c.audio.jitter.frames
