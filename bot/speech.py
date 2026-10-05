"""The user's microphone, as Flux hears it: only their speech, as Silero finds it, from a moment
before each start (so no word is clipped while Silero makes sure), through short pauses, and
silence in between, so music, a cough or the room's noise isn't transcribed.

The game decides only what the player sees of their voice (its ring and waveform), and, while a
character's voice is playing, which its microphone may pick up, sends the microphone only while
the player's speaking over it."""

from __future__ import annotations

from collections import deque
from dataclasses import replace

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.audio.vad.vad_analyzer import VADAnalyzer, VADParams, VADState
from pipecat.frames.frames import Frame, InputAudioRawFrame, StartFrame
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from config import (
    SPEECH_HOLD_S,
    SPEECH_MIN_VOLUME,
    SPEECH_PRE_ROLL_S,
    SPEECH_VAD_CONFIDENCE,
    SPEECH_VAD_START_S,
)


def speech_vad() -> SileroVADAnalyzer:
    """Silero for the gate: quick to open, and slow to close, so pauses mid-sentence stay in."""
    return SileroVADAnalyzer(
        params=VADParams(
            confidence=SPEECH_VAD_CONFIDENCE,
            start_secs=SPEECH_VAD_START_S,
            stop_secs=SPEECH_HOLD_S,
            min_volume=SPEECH_MIN_VOLUME,
        )
    )


def _seconds(frame: InputAudioRawFrame) -> float:
    return len(frame.audio) / max(1, frame.sample_rate * frame.num_channels * 2)


class SpeechGate(FrameProcessor):
    """Passes the user's audio on while they're speaking, and silence of the same length
    otherwise (Flux wants audio all along). Once speech starts, the audio held from just before
    it goes first: a little more audio than time, which Flux takes in its stride."""

    def __init__(self, vad: VADAnalyzer | None = None, pre_roll_s: float = SPEECH_PRE_ROLL_S):
        super().__init__()
        self._vad = vad or speech_vad()
        self._pre_roll_s = pre_roll_s
        self._held: deque[InputAudioRawFrame] = deque()
        self._held_s = 0.0
        self.open = False

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        if isinstance(frame, StartFrame):
            self._vad.set_sample_rate(frame.audio_in_sample_rate)
        if direction != FrameDirection.DOWNSTREAM or not isinstance(frame, InputAudioRawFrame):
            await self.push_frame(frame, direction)
            return
        state = await self._vad.analyze_audio(frame.audio)
        speaking = state in (VADState.SPEAKING, VADState.STOPPING)
        if speaking and not self.open:
            # Its start, from a moment before Silero was sure.
            while self._held:
                await self.push_frame(self._held.popleft(), direction)
            self._held_s = 0.0
        self.open = speaking
        if speaking:
            await self.push_frame(frame, direction)
            return
        # Held a moment, in case it's the start of something; silence goes on in its place.
        self._held.append(frame)
        self._held_s += _seconds(frame)
        while self._held and self._held_s - _seconds(self._held[0]) >= self._pre_roll_s:
            self._held_s -= _seconds(self._held.popleft())
        silent = replace(frame, audio=bytes(len(frame.audio)))
        silent.transport_source = frame.transport_source
        await self.push_frame(silent, direction)
