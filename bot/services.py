"""The session's Pipecat services and clients, built from `Settings`."""

from __future__ import annotations

import time
from collections.abc import Sequence

from loguru import logger
from openai import AsyncOpenAI
from pipecat.classifiers.jev.client import JevClient
from pipecat.frames.frames import (
    Frame,
    InterimTranscriptionFrame,
    ProposedUserStoppedSpeakingFrame,
    TranscriptionFrame,
)
from pipecat.processors.frame_processor import FrameDirection
from pipecat.services.cartesia.tts import CartesiaTTSService
from pipecat.services.deepgram.flux.stt import DeepgramFluxSTTService
from pipecat.services.openai.llm import OpenAILLMService
from pipecat.turns.user_start import MinWordsUserTurnStartStrategy
from pipecat.turns.user_stop import ExternalUserTurnStopStrategy
from pipecat.turns.user_turn_strategies import UserTurnStrategies
from pipecat.utils.time import time_now_iso8601

from config import (
    DEEPGRAM_MODEL,
    JEV_TIMEOUT_S,
    LLM_TEMPERATURE,
    LLM_TOKENS,
    LLM_WARM_TIMEOUT_S,
    MIN_WORDS_TO_INTERRUPT,
    Character,
    Settings,
)


def extra_body(settings: Settings) -> dict:
    """The request body PhoneLLM needs beyond the OpenAI parameters."""
    return {
        "chat_template_kwargs": {"enable_thinking": False},
        "repetition_penalty": settings.llm_repetition_penalty,
    }


class PhoneLLMService(OpenAILLMService):
    """PhoneLLM on its OpenAI-compatible endpoint. Its chat template has no developer role."""

    supports_developer_role = False


def llm(settings: Settings, system_prompt: str) -> OpenAILLMService:
    """One character's LLM, with the character's prompt as its system prompt: PhoneLLM at
    temperature 0, as the kitchen table has it, or OpenAI."""
    if not settings.phonellm:
        return OpenAILLMService(
            api_key=settings.llm_api_key,
            settings=OpenAILLMService.Settings(
                model=settings.llm_model,
                system_instruction=system_prompt,
                max_tokens=LLM_TOKENS,
            ),
        )
    return PhoneLLMService(
        api_key=settings.llm_api_key,
        base_url=settings.llm_base_url,
        settings=OpenAILLMService.Settings(
            model=settings.llm_model,
            system_instruction=system_prompt,
            temperature=LLM_TEMPERATURE,
            max_tokens=LLM_TOKENS,
            extra={"extra_body": extra_body(settings)},
        ),
    )


class FluxSTTService(DeepgramFluxSTTService):
    """Deepgram Flux, with its running transcript of each turn passed on as interim
    transcriptions, as other STT services do. The turn's start strategy counts their words, the
    director reads along with them, and the client shows them.

    Flux reports that transcript only as an event (`on_update`), and event handlers run as tasks,
    so an update pushed from one could land after the turn's final transcript and hold the turn
    open. Here each is pushed in order, as it arrives.

    A turn opens on one of those updates, and the interruption it sends clears the user
    aggregator's queue. A short turn's last update and its end can arrive together, so the
    turn's final transcript and its end are kept through interruptions: they'd be lost
    otherwise, and the turn left open with nothing said."""

    async def _handle_update(self, transcript: str) -> None:
        if transcript:
            frame = InterimTranscriptionFrame(transcript, self._user_id, time_now_iso8601())
            await self.push_frame(frame)
        await super()._handle_update(transcript)

    async def push_frame(
        self, frame: Frame, direction: FrameDirection = FrameDirection.DOWNSTREAM
    ) -> None:
        if isinstance(frame, (TranscriptionFrame, ProposedUserStoppedSpeakingFrame)):
            frame.interruptible = False
        await super().push_frame(frame, direction)


def stt(settings: Settings, cast: Sequence[Character]) -> FluxSTTService:
    """Deepgram Flux: the user's words, and when their turn ends (with `turn_strategies`)."""
    return FluxSTTService(
        api_key=settings.deepgram_api_key,
        settings=FluxSTTService.Settings(model=DEEPGRAM_MODEL, keyterm=[c.name for c in cast]),
    )


def turn_strategies() -> UserTurnStrategies:
    """The user's turns, with Flux (`stt`) deciding when each ends.

    Flux would have its own turn starts open turns too, but in a room of six, a laugh would
    then stop the whole table. Instead, while anyone at the table is talking, it takes a couple
    of words to take the floor: a laugh or an "mm" doesn't. When they're quiet, one word does."""
    return UserTurnStrategies(
        start=[MinWordsUserTurnStartStrategy(min_words=MIN_WORDS_TO_INTERRUPT)],
        stop=[ExternalUserTurnStopStrategy()],
    )


def tts(settings: Settings, character: Character) -> CartesiaTTSService:
    """A character's own voice. Its audio goes to the transport destination named after them:
    their own channel of the game's audio (`mixer.py`), which plays it from their head, so
    several of them can speak at once, each from where they are."""
    return CartesiaTTSService(
        api_key=settings.cartesia_api_key,
        settings=CartesiaTTSService.Settings(voice=character.voice),
        transport_destination=character.id,
    )


def jev(settings: Settings) -> JevClient:
    return JevClient(api_key=settings.jev_api_key, model=settings.jev_model, timeout=JEV_TIMEOUT_S)


async def warm_llm(settings: Settings) -> None:
    """Wake the hosted PhoneLLM endpoint, which scales to zero: a cold start takes minutes.
    (OpenAI needs no waking.)"""
    if not settings.phonellm:
        return
    client = AsyncOpenAI(
        api_key=settings.llm_api_key,
        base_url=settings.llm_base_url,
        timeout=LLM_WARM_TIMEOUT_S,
        max_retries=0,
    )
    started = time.perf_counter()
    try:
        await client.chat.completions.create(
            model=settings.llm_model,
            messages=[{"role": "user", "content": "Say hi."}],
            max_tokens=4,
            temperature=0,
            extra_body={"chat_template_kwargs": {"enable_thinking": False}},
        )
        logger.info(f"PhoneLLM: warm ({time.perf_counter() - started:.1f} s)")
    except Exception as error:  # noqa: BLE001 — the session goes on; its first turn will wait
        logger.warning(
            f"PhoneLLM: warm-up failed after {time.perf_counter() - started:.1f} s: {error}"
        )
    finally:
        await client.close()
