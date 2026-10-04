"""Every character's voice, to the game, in one stream: a channel each.

The game plays each character's voice from their head, so it needs them apart. Over Daily, each
character would have an audio track of their own (a transport destination each); over the
game's WebSocket there's one stream of audio frames. So the output puts each character's voice
on a channel of its own, in the order of the cast (the `cast` message says which is which), and
the game splits them up again:

    maya's TTS ─┐
    theo's TTS ─┼─ CastOutputTransport ─► 3-channel frames ─► the game ─► a voice in each head
    juno's TTS ─┘

Each character's TTS writes its audio to its own destination as fast as it makes it; the output
takes it from each in turn, a block at a time, and sends the blocks together, interleaved, paced
at the speed they play (a little ahead, for the network), so voices said at once play at once.
A character's audio is held once a moment of it is waiting to go, so their TTS keeps pace with
the voice playing: "bot speaking" stays in step with what's heard.

When the user cuts in, every voice is dropped, here and in the game.
"""

from __future__ import annotations

import asyncio
import time
from array import array
from collections.abc import Sequence

from fastapi import WebSocket
from pipecat.frames.frames import (
    CancelFrame,
    EndFrame,
    Frame,
    InterruptionFrame,
    OutputAudioRawFrame,
    StartFrame,
)
from pipecat.processors.frame_processor import FrameDirection
from pipecat.transports.base_transport import BaseTransport
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketClient,
    FastAPIWebsocketOutputTransport,
    FastAPIWebsocketParams,
    FastAPIWebsocketTransport,
)

from config import VOICE_BLOCK_S, VOICE_LEAD_S, VOICE_QUEUE_S

SAMPLE_BYTES = 2  # 16-bit PCM


def interleave(blocks: Sequence[bytes], frames: int) -> bytes:
    """Mono blocks of 16-bit PCM, one per channel, as one interleaved block of `frames` frames;
    a block that's short (or empty) is padded with silence."""
    channels = len(blocks)
    mixed = array("h", bytes(frames * channels * SAMPLE_BYTES))
    for channel, block in enumerate(blocks):
        if not block:
            continue
        samples = array("h")
        samples.frombytes(block[: frames * SAMPLE_BYTES].ljust(frames * SAMPLE_BYTES, b"\0"))
        mixed[channel::channels] = samples
    return mixed.tobytes()


class CastOutputTransport(FastAPIWebsocketOutputTransport):
    """The WebSocket's output, with each character's voice on its own channel."""

    def __init__(
        self,
        transport: BaseTransport,
        client: FastAPIWebsocketClient,
        params: FastAPIWebsocketParams,
        channels: Sequence[str],
        **kwargs,
    ) -> None:
        super().__init__(transport, client, params, **kwargs)
        self._channels = {destination: i for i, destination in enumerate(channels)}
        self._pending = [bytearray() for _ in channels]
        self._arrived = asyncio.Event()  # a voice has audio waiting
        self._taken = asyncio.Event()  # the mixer took some
        self._mixer: asyncio.Task | None = None

    @property
    def channels(self) -> int:
        return len(self._pending)

    async def start(self, frame: StartFrame) -> None:
        await super().start(frame)
        if self._mixer is None:
            self._mixer = self.create_task(self._mix(), "voices")

    async def stop(self, frame: EndFrame) -> None:
        await self._stop_mixer()
        await super().stop(frame)

    async def cancel(self, frame: CancelFrame) -> None:
        await self._stop_mixer()
        await super().cancel(frame)

    async def _stop_mixer(self) -> None:
        if self._mixer is not None:
            await self.cancel_task(self._mixer)
            self._mixer = None
        self._taken.set()

    @property
    def _open(self) -> bool:
        return self._client.is_connected and not self._client.is_closing

    async def write_audio_frame(self, frame: OutputAudioRawFrame) -> bool:
        if not self._open:
            return False
        channel = self._channels.get(frame.transport_destination or "")
        if channel is None:
            return True  # nobody's voice: nothing to play
        pending = self._pending[channel]
        pending.extend(frame.audio)
        self._arrived.set()
        # Held while enough of this voice is waiting: it goes as fast as it plays.
        limit = int(self.sample_rate * VOICE_QUEUE_S) * SAMPLE_BYTES
        while len(pending) > limit and self._open and self._mixer is not None:
            self._taken.clear()
            await self._taken.wait()
        return True

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        if isinstance(frame, InterruptionFrame) and direction == FrameDirection.DOWNSTREAM:
            self.drop(frame.transport_destination)
        await super().process_frame(frame, direction)

    def drop(self, destination: str | None) -> None:
        """Whoever an interruption is for stops: what's waiting of their voice is dropped, or of
        every voice, for an interruption that names nobody."""
        channel = self._channels.get(destination) if destination is not None else None
        for i, pending in enumerate(self._pending):
            if destination is None or i == channel:
                pending.clear()
        self._taken.set()

    async def _write_frame(self, frame: Frame) -> bool:
        # The game drops all the audio it has when it's told of an interruption, every voice's:
        # it's only told of the room's, which stops every voice, not of each character's.
        if isinstance(frame, InterruptionFrame) and frame.transport_destination is not None:
            return True
        return await super()._write_frame(frame)

    async def _mix(self) -> None:
        """Sends the voices a block at a time, every channel's together, paced at the speed
        they play, VOICE_LEAD_S ahead; nothing while nobody's speaking."""
        rate = self.sample_rate
        frames = max(1, int(rate * VOICE_BLOCK_S))
        size = frames * SAMPLE_BYTES
        started: float | None = None
        sent = 0
        while True:
            if not any(self._pending):
                started = None
                self._taken.set()
                self._arrived.clear()
                await self._arrived.wait()
                continue
            now = time.monotonic()
            if started is None:
                started, sent = now, 0
            ahead = sent / rate - (now - started)
            if ahead > VOICE_LEAD_S:
                await asyncio.sleep(ahead - VOICE_LEAD_S)
                continue
            blocks = []
            for pending in self._pending:
                blocks.append(bytes(pending[:size]))
                del pending[:size]
            self._taken.set()
            audio = interleave(blocks, frames)
            frame = OutputAudioRawFrame(audio=audio, sample_rate=rate, num_channels=self.channels)
            await super()._write_frame(frame)
            sent += frames


class CastWebsocketTransport(FastAPIWebsocketTransport):
    """The game's WebSocket, with each character's voice on its own channel of its audio: one
    for each of `channels`, the characters' transport destinations, in order."""

    def __init__(
        self, websocket: WebSocket, params: FastAPIWebsocketParams, channels: Sequence[str]
    ) -> None:
        super().__init__(websocket=websocket, params=params)
        self._output = CastOutputTransport(
            self, self._client, self._params, channels=channels, name=self._output_name
        )
