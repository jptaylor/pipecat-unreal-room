"""The voices to the game, a channel each (no network: what would be sent is kept)."""

from __future__ import annotations

import asyncio
from array import array

import pytest
from pipecat.frames.frames import Frame, InterruptionFrame, OutputAudioRawFrame
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketOutputTransport,
    FastAPIWebsocketParams,
)

from config import VOICE_BLOCK_S
from mixer import CastOutputTransport, interleave

RATE = 24000


def pcm(value: int, seconds: float) -> bytes:
    return array("h", [value] * int(RATE * seconds)).tobytes()


def test_blocks_are_interleaved_a_channel_each_and_padded() -> None:
    mixed = array("h")
    mixed.frombytes(interleave([array("h", [1, 2]).tobytes(), b"", array("h", [3]).tobytes()], 2))
    assert list(mixed) == [1, 0, 3, 2, 0, 0]


class Client:
    is_connected = True
    is_closing = False


@pytest.fixture
def sent(monkeypatch: pytest.MonkeyPatch) -> list[Frame]:
    frames: list[Frame] = []

    async def write(self, frame: Frame) -> bool:
        frames.append(frame)
        return True

    monkeypatch.setattr(FastAPIWebsocketOutputTransport, "_write_frame", write)
    return frames


def output() -> CastOutputTransport:
    transport = CastOutputTransport(
        None,  # type: ignore[arg-type]
        Client(),  # type: ignore[arg-type]
        FastAPIWebsocketParams(),
        channels=["maya", "theo", "juno"],
    )
    transport._sample_rate = RATE
    return transport


def channel(frames: list[Frame], index: int) -> list[int]:
    samples: list[int] = []
    for frame in frames:
        assert isinstance(frame, OutputAudioRawFrame) and frame.num_channels == 3
        mixed = array("h")
        mixed.frombytes(frame.audio)
        samples.extend(mixed[index::3])
    return samples


async def test_voices_said_at_once_play_at_once_each_on_its_own_channel(
    sent: list[Frame],
) -> None:
    transport = output()
    mixer = asyncio.create_task(transport._mix())
    transport._mixer = mixer
    maya = OutputAudioRawFrame(pcm(100, 0.05), RATE, 1)
    maya.transport_destination = "maya"
    juno = OutputAudioRawFrame(pcm(-200, 0.05), RATE, 1)
    juno.transport_destination = "juno"
    await asyncio.gather(transport.write_audio_frame(maya), transport.write_audio_frame(juno))
    await asyncio.sleep(0.25)
    mixer.cancel()
    block = int(RATE * VOICE_BLOCK_S)
    assert channel(sent, 0)[: 2 * block] == [100] * (2 * block)
    assert channel(sent, 1)[: 2 * block] == [0] * (2 * block)
    assert channel(sent, 2)[: 2 * block] == [-200] * (2 * block)


async def test_a_voice_is_paced_at_the_speed_it_plays(sent: list[Frame]) -> None:
    transport = output()
    mixer = asyncio.create_task(transport._mix())
    transport._mixer = mixer
    theo = OutputAudioRawFrame(pcm(7, 1.0), RATE, 1)
    theo.transport_destination = "theo"
    write = asyncio.create_task(transport.write_audio_frame(theo))
    await asyncio.sleep(0.3)
    # About 0.3 s played, a little more sent ahead; the rest held back.
    played = len(channel(sent, 1)) / RATE
    assert 0.25 < played < 0.6
    assert not write.done()
    mixer.cancel()
    write.cancel()


async def test_cutting_in_drops_what_was_waiting_to_be_said(sent: list[Frame]) -> None:
    transport = output()
    for name in ("maya", "theo"):
        transport._pending[transport._channels[name]].extend(pcm(1, 0.05))
    transport.drop("maya")  # one character's line stops: only theirs goes
    maya, theo = (transport._pending[transport._channels[n]] for n in ("maya", "theo"))
    assert not maya and theo
    transport.drop(None)  # the user cut in: every voice goes
    assert not theo
    # The game isn't told of one character's interruption: it would drop every voice.
    cut = InterruptionFrame()
    cut.transport_destination = "maya"
    assert await transport._write_frame(cut) is True and sent == []
