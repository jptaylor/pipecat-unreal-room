"""The user's turns: Deepgram Flux says when each ends (`services.stt`), and it takes a couple of
words to take the floor while the table talks (`services.turn_strategies`). No network: Flux's
messages are handed to the service as if they'd come over its socket."""

from __future__ import annotations

import pytest
from pipecat.frames.frames import (
    BotStartedSpeakingFrame,
    Frame,
    InterimTranscriptionFrame,
    LLMContextFrame,
    ProposedUserStoppedSpeakingFrame,
    TranscriptionFrame,
)
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import (
    LLMUserAggregator,
    LLMUserAggregatorParams,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.tests.utils import SleepFrame, run_test

import services
from config import Settings, load_cast
from director import user_text

CAST = load_cast()
SETTINGS = Settings(
    llm_api_key="k",
    llm_base_url=None,
    llm_model="m",
    phonellm=False,
    jev_api_key="k",
    jev_model="jev",
    cartesia_api_key="k",
    deepgram_api_key="k",
)


async def flux(*events: tuple[str, str]) -> list[Frame]:
    """What our Flux service sends on down the pipeline for these Flux events."""
    stt = services.stt(SETTINGS, CAST)
    sent: list[Frame] = []

    async def capture(self, frame: Frame, direction: FrameDirection = FrameDirection.DOWNSTREAM):
        if direction == FrameDirection.DOWNSTREAM:
            sent.append(frame)

    async def nothing(*args, **kwargs) -> None:
        pass

    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(FrameProcessor, "push_frame", capture)
        patch.setattr(stt, "stop_ttfb_metrics", nothing)  # not started: no metrics
        patch.setattr(stt, "emit_stt_usage_metrics", nothing)
        for event, words in events:
            await stt._handle_turn_info({"event": event, "transcript": words})
    return sent


def said(*words: str) -> list[tuple[str, str]]:
    """Flux's events for a turn: its start, its running transcript, and its end."""
    *running, final = words
    return [("StartOfTurn", running[0] if running else final)] + [
        *(("Update", w) for w in running),
        ("EndOfTurn", final),
    ]


async def test_flux_passes_its_running_transcript_on_ahead_of_the_final_one() -> None:
    sent = await flux(*said("Who wants", "Who wants cake", "Who wants cake?"))
    texts = [
        (type(f).__name__, getattr(f, "text", None))
        for f in sent
        if isinstance(f, (InterimTranscriptionFrame, TranscriptionFrame))
    ]
    assert texts == [
        ("InterimTranscriptionFrame", "Who wants"),
        ("InterimTranscriptionFrame", "Who wants cake"),
        ("TranscriptionFrame", "Who wants cake?"),
    ]
    assert isinstance(sent[-1], ProposedUserStoppedSpeakingFrame)
    # Neither the words nor the end of the turn can be lost to an interruption.
    assert [f.interruptible for f in sent[-2:]] == [False, False]


async def turns(*frames: Frame) -> list[str]:
    """The user turns a user aggregator with our strategies makes of these frames, all sent at
    once (as a short turn's can arrive)."""
    params = LLMUserAggregatorParams(user_turn_strategies=services.turn_strategies())
    aggregator = LLMUserAggregator(LLMContext(), params=params)
    # Long enough for a turn to end on Flux's word, and far short of the 5 s watchdog.
    down, _ = await run_test(aggregator, frames_to_send=[*frames, SleepFrame(0.3)])
    return [user_text(f.context.get_messages()[-1]) for f in down if isinstance(f, LLMContextFrame)]


async def test_a_turn_ends_when_flux_says_it_has() -> None:
    sent = await flux(*said("Who wants", "Who wants cake", "Who wants cake?"))
    assert await turns(*sent) == ["Who wants cake?"]


async def test_a_one_word_turn_isnt_lost_to_the_interruption_it_makes() -> None:
    sent = await flux(*said("Yes", "Yes."))
    assert await turns(*sent) == ["Yes."]


async def test_a_laugh_while_the_table_talks_doesnt_take_the_floor() -> None:
    sent = await flux(*said("Haha", "Haha."))
    assert await turns(BotStartedSpeakingFrame(), *sent) == []


async def test_a_couple_of_words_while_the_table_talks_do() -> None:
    sent = await flux(*said("Hang", "Hang on", "Hang on."))
    assert await turns(BotStartedSpeakingFrame(), *sent) == ["Hang on."]
