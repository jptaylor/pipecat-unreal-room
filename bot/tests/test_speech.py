"""The speech gate in front of Flux (`speech.py`)."""

from __future__ import annotations

from typing import Any

from pipecat.audio.vad.vad_analyzer import VADState
from pipecat.frames.frames import InputAudioRawFrame
from pipecat.processors.frame_processor import FrameDirection

from speech import SpeechGate

RATE = 16000


class ScriptedVAD:
    """A VAD that says what it's told, a chunk at a time."""

    def __init__(self, states: list[VADState]) -> None:
        self.states = states

    def set_sample_rate(self, sample_rate: int) -> None:
        pass

    async def analyze_audio(self, buffer: bytes) -> VADState:
        return self.states.pop(0)


async def gate_through(states: list[VADState], pre_roll_s: float) -> list[int]:
    """What the gate passes on, for chunks the VAD reads as `states`: each chunk's first byte,
    its number (0: silence)."""
    gate = SpeechGate(ScriptedVAD(states), pre_roll_s=pre_roll_s)  # type: ignore[arg-type]
    out: list[int] = []

    async def push(frame: Any, direction: FrameDirection = FrameDirection.DOWNSTREAM) -> None:
        out.append(frame.audio[0])

    gate.push_frame = push  # type: ignore[method-assign]
    for number in range(1, len(states) + 1):
        audio = bytes([number, 0]) * (RATE // 50)  # 20 ms
        frame = InputAudioRawFrame(audio=audio, sample_rate=RATE, num_channels=1)
        await gate.process_frame(frame, FrameDirection.DOWNSTREAM)
    return out


async def test_speech_goes_through_from_just_before_it_starts_and_silence_otherwise(
    monkeypatch,
) -> None:
    async def nothing(*args: Any, **kwargs: Any) -> None:
        pass

    monkeypatch.setattr("pipecat.processors.frame_processor.FrameProcessor.process_frame", nothing)
    quiet, speaking = VADState.QUIET, VADState.SPEAKING
    states = [quiet, quiet, quiet, VADState.STARTING, speaking, VADState.STOPPING, speaking]
    out = await gate_through([*states, quiet, quiet], pre_roll_s=0.035)  # two chunks held
    # Silence while it's quiet (or not yet sure), then the two chunks from just before the
    # speech, the speech, its pause kept in, and silence again.
    assert out == [0, 0, 0, 0, 3, 4, 5, 6, 7, 0, 0]
