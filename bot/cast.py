"""The characters: their prompts, and `CharacterWorker`, the worker that speaks as one.

Each character is a worker of its own, with its own LLM and its own TTS (its voice, sent to its
own transport destination, so it plays on its own channel of the game's audio, from its head):

    bridge in → LLM → Script → TTS → Stamp → bridge out

A line arrives as a `speak` job from the room: the character's view of the whole conversation,
and a take id. The worker writes the line (sending it back as a job update as soon as it's
written, so the room can plan what follows while it's still being voiced), voices it, and
answers the job once the TTS is done. Every frame it sends carries the take id, so the room can
hold a line until its turn and drop anything from a line it has abandoned. Cancelling the job
interrupts the worker.

Characters take no frames from the bus: with several of them active at once, each would echo
the others' frames back onto it. Their lines come as jobs, and they only ever send.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import Sequence
from pathlib import Path
from typing import Any, cast

from loguru import logger
from pipecat.bus import BusJobCancelMessage, BusJobRequestMessage
from pipecat.bus.messages import BusFrameMessage, BusMessage
from pipecat.frames.frames import (
    Frame,
    InterruptionFrame,
    LLMContextFrame,
    LLMFullResponseEndFrame,
    LLMFullResponseStartFrame,
    LLMTextFrame,
    TTSStoppedFrame,
)
from pipecat.pipeline.job_context import JobStatus
from pipecat.pipeline.job_decorator import job
from pipecat.pipeline.pipeline import Pipeline
from pipecat.processors.aggregators.llm_context import LLMContext, LLMContextMessage
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.services.llm_service import LLMService
from pipecat.services.tts_service import TTSService
from pipecat.workers.llm import LLMWorker

from config import SPEAK_JOB, VOICE_TIMEOUT_S, WRITE_TIMEOUT_S, Character
from room import SILENT, could_be_silent, is_silent

PROMPTS = Path(__file__).resolve().parent / "prompts"
COUNTS = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven"}
TAKE = "take"  # the frame metadata key: which line a frame belongs to


def prompt(me: Character, cast: Sequence[Character]) -> str:
    """`me`'s system prompt: `prompts/character.md`, with `prompts/<id>.md` as their persona and
    everyone else at the table listed."""
    persona_file = PROMPTS / f"{me.id}.md"
    persona = persona_file.read_text().strip() if persona_file.exists() else ""
    others = [c for c in cast if c.id != me.id]
    values = {
        "name": me.name,
        "role": me.role.lower(),
        "tagline": me.tagline,
        "colour": me.colour,
        "place": me.place or "the house",
        "count": COUNTS.get(len(others), str(len(others))),
        "others": "\n".join(f"- {c.brief()}" for c in others),
        "example": others[0].name,
    }
    values["persona"] = fill(persona, values)
    return fill((PROMPTS / "character.md").read_text().strip(), values)


def fill(template: str, values: dict[str, str]) -> str:
    return re.sub(r"\{\{\s*(\w+)\s*\}\}", lambda m: values[m.group(1)], template)


def take_of(frame: Frame) -> int | None:
    """The line a character's frame belongs to (None: no line, or one abandoned)."""
    return frame.metadata.get(TAKE)


class Script(FrameProcessor):
    """Between the LLM and the TTS: the line as it's written, reported once it's whole.

    A character may choose to stay quiet (in a chorus, "say yes if you want some" when they
    don't) by writing only `SILENT`. The first words of every line are held back until they
    can't be that, so it never reaches the TTS.

    Each response belongs to the take it started for: the end of a response cut short (an
    abandoned line, ending after the next line's job has begun) mustn't be taken for the new
    line, written as nothing.

    What a character does, when they describe it *like this* (they're told not to, but do,
    especially as they set about something), isn't said: it's left out of the line.
    """

    def __init__(self, worker: CharacterWorker, name: str) -> None:
        super().__init__(name=f"{name}::Script")
        self._worker = worker
        self._take: int | None = None  # the take the response being written is for
        self._text: list[str] = []
        self._held: list[Frame] | None = None  # the line so far, while it could be SILENT
        self._acting = False  # between asterisks: what they do, not what they say

    def _said(self, text: str) -> str:
        """The part of some of the line that's said, leaving out what's *done*."""
        said = []
        for part in re.split(r"(\*)", text):
            if part == "*":
                self._acting = not self._acting
            elif not self._acting:
                said.append(part)
        return "".join(said)

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        if isinstance(frame, LLMFullResponseStartFrame):
            self._take, self._text, self._held = self._worker.take, [], []
            self._acting = False
        elif isinstance(frame, LLMTextFrame):
            frame.text = self._said(frame.text)
            if not frame.text:
                return
            self._text.append(frame.text)
            if self._held is not None:
                self._held.append(frame)
                if not could_be_silent("".join(self._text)):
                    await self._release()
                return
        elif isinstance(frame, LLMFullResponseEndFrame):
            text = re.sub(r"\s{2,}", " ", "".join(self._text)).strip()
            take, self._take = self._take, None
            if take is None or take != self._worker.take:
                self._held = None  # the end of a response cut short: not this line's
            elif is_silent(text):
                self._held = None  # never voiced
                self._worker.written(SILENT)
            else:
                await self._release()
                self._worker.written(text)
        elif isinstance(frame, InterruptionFrame):
            self._take, self._text, self._held = None, [], None
            self._acting = False
        await self.push_frame(frame, direction)

    async def _release(self) -> None:
        held, self._held = self._held or [], None
        for frame in held:
            await self.push_frame(frame)


class Stamp(FrameProcessor):
    """After the TTS: each frame is stamped with the line it belongs to. The line's runs of
    TTS audio are counted (each ends with a TTSStoppedFrame), and the first run to end after the
    whole line was written means it's voiced."""

    def __init__(self, worker: CharacterWorker, name: str) -> None:
        super().__init__(name=f"{name}::Stamp")
        self._worker = worker

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        frame.metadata[TAKE] = self._worker.take
        await self.push_frame(frame, direction)
        if isinstance(frame, TTSStoppedFrame) and frame.metadata[TAKE] is not None:
            self._worker.stopped()


class CharacterWorker(LLMWorker):
    """Speaks as one character: one line per `speak` job, one job at a time."""

    def __init__(self, character: Character, llm: LLMService[Any], tts: TTSService) -> None:
        self.character = character
        self.take: int | None = None  # the line being written and voiced
        self._written: asyncio.Future[str] | None = None
        self._voiced = asyncio.Event()
        self._stops = 0  # runs of TTS audio the line has had, each ending in a TTSStoppedFrame
        self._cancelled_early: set[str] = set()  # jobs cancelled before their request arrived
        name = character.id
        pipeline = Pipeline([llm, Script(self, name), tts, Stamp(self, name)])
        super().__init__(name, llm=llm, pipeline=pipeline, bridged=(), active=True)

    def accepts_bus_message(self, message: BusMessage) -> bool:
        return not isinstance(message, BusFrameMessage) and super().accepts_bus_message(message)

    async def on_bus_message(self, message: BusMessage) -> None:
        # A cancel travels ahead of data on the bus, so it can overtake its own request: one
        # for a job not seen yet is remembered, and that job is skipped when it comes.
        if (
            isinstance(message, BusJobCancelMessage)
            and message.target == self.name
            and message.job_id not in self.active_jobs
        ):
            self._cancelled_early.add(message.job_id)
        await super().on_bus_message(message)

    @job(name=SPEAK_JOB, sequential=True)
    async def speak(self, message: BusJobRequestMessage) -> None:
        if message.job_id in self._cancelled_early:
            self._cancelled_early.discard(message.job_id)
            await self.send_job_response(message.job_id, status=JobStatus.CANCELLED)
            return
        payload = message.payload or {}
        messages = cast(list[LLMContextMessage], payload["messages"])
        self.take = payload["take"]
        self._written = asyncio.get_running_loop().create_future()
        self._voiced.clear()
        self._stops = 0
        name = self.character.name
        logger.debug(f"{name}: take {self.take} ({len(messages)} messages)")
        try:
            await self.queue_frame(LLMContextFrame(context=LLMContext(messages=messages)))
            try:
                text = await asyncio.wait_for(asyncio.shield(self._written), WRITE_TIMEOUT_S)
            except TimeoutError:
                logger.warning(f"{name}: nothing written in {WRITE_TIMEOUT_S} s")
                await self.queue_frame(InterruptionFrame())
                text = ""
            if message.job_id not in self.active_jobs:
                return  # the room has gone (the session ended)
            await self.send_job_update(message.job_id, {"text": text})
            if text.strip() and not is_silent(text):
                try:
                    await asyncio.wait_for(self._voiced.wait(), VOICE_TIMEOUT_S)
                except TimeoutError:
                    logger.warning(f"{name}: the TTS never said it was done (take {self.take})")
            if message.job_id in self.active_jobs:
                await self.send_job_response(message.job_id, {"text": text, "stops": self._stops})
        except asyncio.CancelledError:
            # The room abandoned the line. Stop the LLM and the TTS now, before this job lets
            # the next one start: an interruption sent any later would cut that one instead.
            # Anything still on its way out carries no take, so the room drops it.
            self.take = None
            await self.queue_frame(InterruptionFrame())
            logger.debug(f"{name}: line abandoned")
            raise
        finally:
            self.take = None
            self._written = None

    def written(self, text: str) -> None:
        if self._written is not None and not self._written.done():
            self._written.set_result(text)

    def stopped(self) -> None:
        """A run of the line's TTS audio has ended; the first after it was all written means
        it's voiced."""
        self._stops += 1
        if self._written is not None and self._written.done():
            self._voiced.set()
