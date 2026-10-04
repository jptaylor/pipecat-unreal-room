"""`Director`: who speaks next in the room, and the processors that let it hear and steer.

The room worker's pipeline:

    transport.input → STT → Hearing → user aggregator → Router → CastBridge → FloorGate
                    → transport.output → FloorEar

- `Hearing` passes the user's words, partial and final, to the director as they are heard, so
  Jev reads who they are talking to while they are still talking (a frontrun: the final read of
  the same words is then a cache hit).
- `Router` takes each finished user turn (the aggregator's `LLMContextFrame`) out of the stream:
  the director asks Jev about it and plays out the engine's plan (`engine.py`).
- `CastBridge`, `FloorGate` and `FloorEar` are the floor (`floor.py`): the characters' voices
  come in over the bus, and each line plays when its turn comes, on its speaker's own channel.

Each line is a *take*: a `speak` job sent to the character's worker (`cast.py`) with their view
of the conversation, and a cue on the floor saying what it waits for. The worker reports the line
as soon as it's written, while the TTS is still voicing it, and the director plans what follows
right away: the next of a group, or (asking Jev) whether someone answers it, or reacts to it. So
the next line is written and voiced while this one plays, and comes in a beat after it ends.

The user is the host, and always has the floor when they take it: speaking (past a few words, so
a laugh doesn't count), typing, or the client's buttons cancel every take still to come and stop
every voice. When they say nothing for a while, the table may fill the silence (the user
aggregator's idle event), as the engine allows.

Everything the director decides goes to the client as RTVI server messages: `jev` (each reading),
`turn` (a take: who, why, who to, and the view they were given), `line` (the transcript, as each
line starts playing, and again if it's cut short or taken back), `voices` (whose audio is
playing) and `cast`.

In the house, the game tells the director where everyone is and who hears whom (`space.py`): the
user's words go only to those who heard them, and each line only to those within earshot of its
speaker. It tells the game how the characters feel and what they do as they speak and listen
(`emote`: a mood, and a gesture, as Jev reads each line), and where they go when the user asks
(`move`: follow, wait, come over, go home, or go somewhere). When the user first comes up to a
character, they say hi.
"""

from __future__ import annotations

import asyncio
import itertools
import random
import time
from collections import deque
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from typing import Any

from loguru import logger
from pipecat.frames.frames import (
    Frame,
    InterimTranscriptionFrame,
    LLMContextFrame,
    TranscriptionFrame,
    UserIdleTimeoutUpdateFrame,
    UserStartedSpeakingFrame,
    UserStoppedSpeakingFrame,
    VADUserStartedSpeakingFrame,
    VADUserStoppedSpeakingFrame,
)
from pipecat.pipeline.job_context import JobError, JobGroupEvent, JobParams
from pipecat.pipeline.worker import PipelineWorker
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from config import (
    ACT_FLOOR,
    ALSO_ASKED_FLOOR,
    CHORUS_STAGGER_S,
    DOING_FLOOR,
    EVENT_STALE_S,
    FOR_FLOOR,
    GESTURE_FLOOR,
    INTENT_FLOOR,
    LISTENER_MOOD_STRENGTH,
    MERGE_GAP_S,
    MOOD_FLOOR,
    MORE_MAX_S,
    MOVE_FLOOR,
    RECENCY_WEIGHT,
    ROUTE_WAIT_S,
    SPEAK_JOB,
    SPEAK_TIMEOUT_S,
    SPECULATE_FLOOR,
    TRANSCRIPT_LAG_S,
    TURN_GAP_S,
    Character,
)
from engine import Engine, Next, Plan, Take, strongest
from floor import Floor
from room import (
    ANSWER,
    GO,
    GROUP,
    HOME,
    NOTE,
    NOTE_ACT,
    NOTE_ACT_FOR,
    NOTE_EVENT,
    NOTE_FLOWER_COLOR,
    NOTE_HAPPENED,
    NOTE_INTRODUCE_HERE,
    NOTE_INTRODUCE_WHOM,
    NOTE_MOVE,
    NOTE_NO_MUSIC,
    SINGULAR_ACTS,
    USER,
    Line,
    Reading,
    Referee,
    Transcript,
    flower_color,
    is_silent,
    normalize,
    top,
    weigh,
)
from space import Space


def user_text(message: Any) -> str:
    """What the user said in one of the aggregator's context messages ("" for anything else)."""
    if not isinstance(message, dict) or message.get("role") != "user":
        return ""
    content = message.get("content")
    return content if isinstance(content, str) else ""


def same_plan(guessed: Plan, final: Plan) -> bool:
    """Whether the final plan is the one guessed: the same lines, from the same characters, for
    the same reasons (in any order, if they're said at once)."""

    def lines(plan: Plan) -> list[tuple[str, str, str | None]]:
        said = [(t.speaker, t.reason, t.note) for t in plan.takes]
        return sorted(said) if plan.together else said

    return (guessed.why, guessed.together) == (final.why, final.together) and lines(
        guessed
    ) == lines(final)


@dataclass
class Early:
    """Lines started on Jev's read-along guess, held (by `hold`) until the final read agrees."""

    plan: Plan
    lives: list[Live]
    hold: int


@dataclass(eq=False)
class Live:
    """A take: a line asked of a character, from being written to having played."""

    take: int
    plan: Take
    epoch: int
    chorus: int | None = None
    line: Line | None = None  # in the transcript once written
    task: asyncio.Task | None = None
    prev: Live | None = None  # the line it follows, in a run of lines in turn
    heard: asyncio.Event = field(default_factory=asyncio.Event)  # set once it starts playing
    started: bool = False  # its audio has started playing
    # Set unless it was started early, on Jev's read-along guess: nothing follows such a line
    # until the final read has confirmed it.
    confirmed: asyncio.Event = field(default_factory=asyncio.Event)
    reading: Reading | None = None  # Jev's read of the line, for how the speaker shows it
    shown: bool = False  # ...once it's been shown

    @property
    def speaker(self) -> str:
        return self.plan.speaker


class Director:
    """Decides who speaks, and keeps the one transcript every character is shown."""

    def __init__(self, cast: Sequence[Character], referee: Referee) -> None:
        self.cast = {c.id: c for c in cast}
        self.transcript = Transcript(cast)
        self.space = Space(cast)
        # What happened in the house, still to be reacted to, and when it happened (monotonic).
        self._events: list[tuple[float, Plan]] = []
        self.engine = Engine(self.transcript)
        self.referee = referee
        self.floor = Floor(self, list(self.cast))
        self.worker: PipelineWorker | None = None  # the room worker, set once it exists

        self._lives: dict[int, Live] = {}
        self._takes = itertools.count(1)
        self._choruses = itertools.count(1)
        self._users = itertools.count(1)
        self._queue: list[Take] = []  # a plan's takes in turn, each asked once the last is written
        self._decided: set[int] = set()  # choruses whose follow-up has been decided
        self._epoch = 0  # moves on whenever the user takes the floor: older plans are void
        self._quiet_since = time.monotonic()
        self._idle_timeout: float | None = None

        self._seen = 0  # messages of the aggregator's context already read
        self._turns = 0  # user turns routed: a frontrun read landing after its turn is dropped
        self._finals: list[str] = []  # the user's words so far this turn
        self._interim = ""
        self._preview_want: str | None = None
        self._preview_task: asyncio.Task | None = None
        self._guess: Reading | None = None  # Jev's latest read of the words still being spoken
        self._last_user: Line | None = None  # the user's last turn, and its id for the client...
        self._last_user_id = ""
        self._last_user_end = 0.0  # ...and when its words ended, and it was routed (monotonic)
        self._last_user_at = 0.0
        # The user's voice (VAD): whether they're audible now, and when it last started and
        # stopped. It leads the transcript, and so the turn, by up to a second.
        self._voice = False
        self._voice_began = 0.0
        self._voice_ended = 0.0
        self._voice_starts: deque[float] = deque(maxlen=16)
        self._turn_began = 0.0  # when the user's current (or last) turn started
        self._wake = asyncio.Event()  # the voice changed, or the floor was taken
        self._resume: asyncio.Task | None = None  # lets lines start again after a voice stops
        self._user_speaking = False
        self._tasks: set[asyncio.Task] = set()

    # --- Wiring --------------------------------------------------------------------------------

    def spawn(self, coro: Any, name: str) -> asyncio.Task:
        task = asyncio.create_task(coro, name=name)
        self._tasks.add(task)
        task.add_done_callback(self._settled)
        return task

    def _settled(self, task: asyncio.Task) -> None:
        self._tasks.discard(task)
        if not task.cancelled() and task.exception() is not None:
            logger.opt(exception=task.exception()).error(f"Director: {task.get_name()} failed")

    async def emit(self, data: dict[str, Any]) -> None:
        if self.worker is not None and self.worker.rtvi is not None:
            await self.worker.rtvi.send_server_message(data)

    async def close(self) -> None:
        for task in list(self._tasks):
            task.cancel()
        await self.referee.close()

    async def send_cast(self) -> None:
        await self.emit(
            {
                "type": "cast",
                "characters": [
                    {"id": c.id, "name": c.name, "role": c.role} for c in self.cast.values()
                ],
                # Whose voice is on which of the audio's channels.
                "channels": list(self.cast),
                "jev": self.referee.model,
            }
        )

    @property
    def busy(self) -> bool:
        return bool(self._lives or self._queue)

    # --- Plans and takes -----------------------------------------------------------------------

    async def welcome(self) -> None:
        """The game is listening. In the house, nobody's near the user yet: each character says
        hi once the user comes up to them (`met`)."""
        await self.send_cast()

    # --- The house, as the game tells it -------------------------------------------------------

    def world(self, data: dict[str, Any]) -> None:
        self.space.world(data)
        self.referee.set_areas(dict(self.space.areas))
        self._where()

    def update_space(self, data: dict[str, Any]) -> None:
        self.space.update(data)
        self._where()

    def earshot(self, data: dict[str, Any]) -> None:
        self.space.earshot(data)

    def _where(self) -> None:
        """Where everyone is, and where each character lives, for Jev."""
        where = self.space.for_jev()
        if self.space.areas:
            where["homes"] = {
                c.name: self.space.area_name(c.home) for c in self.cast.values() if c.home
            }
        self.transcript.where = where

    async def met(self, character: str) -> None:
        """The user has come up to `character` for the first time. They say nothing yet: they
        wait to be spoken to."""
        if character in self.cast:
            logger.info(f"Director: the user came up to {self.cast[character].name}")

    def _talked_with(self, characters: Iterable[str]) -> None:
        """The user and these characters have spoken to each other: they've met, and Jev knows."""
        if any([self.space.talked_with(c) for c in characters]):
            self._where()

    async def _news(self) -> None:
        """Whatever happened while the table was talking, the latest first, if it's still news."""
        if self.busy or self._user_speaking:
            return
        while self._events:
            at, plan = self._events.pop()
            if time.monotonic() - at < EVENT_STALE_S:
                await self.play(plan)
                return

    async def event(self, data: dict[str, Any]) -> None:
        """Something happened in the house (`space.py`): whoever saw or heard it knows, and
        whoever it's most about says something, as soon as nobody else is talking."""
        kind = str(data.get("kind") or "")
        if kind == "music":
            kind = "music_on" if data.get("on") else "music_off"
        heard_by = {str(h) for h in data.get("heard_by") or []}
        heard = [c for c in self.cast if c in heard_by]
        who, to = str(data.get("who") or ""), str(data.get("to") or "")
        fields = {
            "who": self.cast[who].name if who in self.cast else "the person",
            "to": self.cast[to].name if to in self.cast else "the person",
            "item": str(data.get("item") or "something"),
            "place": str(data.get("place") or "the house"),
        }
        if kind in NOTE_HAPPENED:
            self.transcript.add(
                NOTE, NOTE_HAPPENED[kind].format(**fields), heard_by=frozenset(heard_by)
            )
        # Who says something about it, if anyone.
        speaker, target = None, USER
        said = kind
        if kind == "gift" and to in self.cast:
            speaker = to
        elif kind == "handed" and to in self.cast and to in heard_by:
            # Handed to one of them: whoever got it says something, to whoever gave it.
            speaker, target, said = to, who if who in self.cast else USER, "received"
        elif kind in ("handed", "baked") and who in self.cast:
            speaker = who
        elif kind == "visit" and who in self.cast and to in self.cast:
            speaker, target = who, to
            self.engine.fresh()
        elif kind == "introduce" and who in self.cast and to in self.cast:
            # They've brought the user to meet someone: they introduce them, and they've met.
            speaker, target = who, to
            self.engine.fresh()
            self._talked_with([to])
        elif kind == "picked":
            owner = str(data.get("owner") or "")
            speaker = owner if owner in heard else None
        elif kind in ("music_on", "music_off", "piano", "wish") and heard:
            # Whoever it's most about: the musician, for music.
            keen = [c for c in heard if "music" in self.cast[c].topics]
            speaker = keen[0] if keen and kind != "wish" else heard[0]
        if kind == "bell":
            # Everyone drops what they were doing, and whatever they'd have said about it.
            takes = [Take(c, "event", NOTE_EVENT["bell"]) for c in self.cast]
            plan = Plan(takes, together=True, why="bell")
            self._events.clear()
        elif speaker is not None and said in NOTE_EVENT:
            note = NOTE_EVENT[said].format(**fields)
            plan = Plan([Take(speaker, "event", note, target)], why=said)
        else:
            return
        logger.info(f"Director: {kind} ({', '.join(t.speaker for t in plan.takes)} to react)")
        self._events.append((time.monotonic(), plan))
        self._events = self._events[-3:]
        await self._news()

    async def play(
        self,
        plan: Plan,
        after: frozenset[int] = frozenset(),
        *,
        gap: float = TURN_GAP_S,
        early: bool = False,
    ) -> list[Live]:
        """Start a plan: its first take now and the rest in turn, or all of them at once.
        Returns the takes asked for now."""
        if not plan.takes:
            return []
        if plan.together:
            # Spread over CHORUS_STAGGER_S from the first voice, in no particular order.
            chorus = next(self._choruses)
            delays = sorted(random.uniform(0.0, CHORUS_STAGGER_S) for _ in plan.takes)
            delays[0] = 0.0
            random.shuffle(delays)
            return [
                await self.ask(
                    take, after=set(after), gap=0.0, delay=delay, chorus=chorus, early=early
                )
                for take, delay in zip(plan.takes, delays, strict=True)
            ]
        self._queue = list(plan.takes[1:])
        return [await self.ask(plan.takes[0], after=set(after), gap=gap, early=early)]

    async def ask(
        self,
        take: Take,
        *,
        after: set[int],
        gap: float = TURN_GAP_S,
        delay: float = 0.0,
        chorus: int | None = None,
        prev: Live | None = None,
        early: bool = False,
    ) -> Live:
        """Ask a character for a line: written now, played once `after` have finished. An
        `early` line (started on a guess) leads to nothing until it's confirmed."""
        assert self.worker is not None
        live = Live(next(self._takes), take, self._epoch, chorus, prev=prev)
        if not early:
            live.confirmed.set()
        self._lives[live.take] = live
        messages = self._view(take)
        await self.floor.expect(
            live.take, take.speaker, after=after, gap=gap, delay=delay, chorus=chorus
        )
        live.task = self.spawn(self._speak(live, messages), f"take {live.take}")
        name = self.cast[take.speaker].name
        note = f": {take.note}" if take.note else ""
        logger.info(f"Director: {name}, take {live.take} ({take.reason}{note})")
        await self.emit(
            {
                "type": "turn",
                "take": live.take,
                "speaker": take.speaker,
                "reason": take.reason,
                "note": take.note,
                "target": take.to,
                "messages": messages,
                "at": time.time(),
            }
        )
        return live

    def _view(self, take: Take) -> list[dict[str, str]]:
        """What `take`'s character is shown: what they heard, and the moment's note, with where
        they are and who's with them first."""
        note = " ".join(n for n in (self.space.situation(take.speaker), take.note) if n)
        return self.transcript.view(take.speaker, note or None)

    async def _speak(self, live: Live, messages: list[dict[str, str]]) -> None:
        assert self.worker is not None
        params = JobParams(
            name=SPEAK_JOB,
            payload={"take": live.take, "messages": messages},
            timeout=SPEAK_TIMEOUT_S,
        )
        try:
            async with self.worker.job(live.speaker, params=params) as job:
                async for event in job:
                    if event.type == JobGroupEvent.UPDATE and event.data is not None:
                        await self.written(live, str(event.data.get("text", "")))
        except JobError as error:
            logger.warning(f"Director: take {live.take} failed: {error}")
            await self._unheard(live)
            return
        except asyncio.CancelledError:
            logger.debug(f"Director: take {live.take} cancelled")
            raise
        stops = int(job.response.get("stops", 0))
        logger.debug(f"Director: take {live.take} voiced ({stops} runs)")
        await self.floor.complete(live.take, stops)

    async def written(self, live: Live, text: str) -> None:
        """A take's line is written (it's being voiced, and may be waiting for its turn)."""
        text = normalize(text)
        if live.epoch != self._epoch or live.take not in self._lives:
            return
        if not text or is_silent(text):
            if text:
                logger.info(f"Director: {self.cast[live.speaker].name} stays quiet")
            else:
                logger.warning(f"Director: {live.speaker} wrote nothing (take {live.take})")
            await self._unheard(live)
            return
        # Heard by those within earshot of its speaker as it plays.
        heard_by = frozenset(self.space.listeners(live.speaker))
        live.line = self.transcript.add(
            live.speaker, text, how=live.plan.how, chorus=live.chorus, heard_by=heard_by
        )
        if live.plan.to == USER and USER in heard_by:
            self._talked_with([live.speaker])
        if live.started:
            # Its first words were voiced, and started playing, before the rest was written.
            await self.emit_line(f"t{live.take}", live.line)
        self.spawn(self._express(live), f"express {live.take}")
        self.spawn(self._follow(live), f"follow {live.take}")

    async def _express(self, live: Live) -> None:
        """Jev's read of a line (the same read that decides what follows it, so it's asked
        once): the mood and gesture its speaker says it with, shown as it plays."""
        line = live.line
        if line is None:
            return
        live.reading = await self.read(line)
        if live.started:
            await self._show(live)

    async def _show(self, live: Live) -> None:
        """The speaker's mood and gesture as their line plays, and how those hearing it take it."""
        reading, line = live.reading, live.line
        if live.shown or reading is None or line is None or reading.error:
            return
        live.shown = True
        # What they set about doing, of their own accord, as they say it.
        doing = reading.does(DOING_FLOOR)
        if doing == "dance" and not self._dance_music():
            doing = None  # nothing to dance to yet
        if doing and live.plan.act is None and live.plan.reason not in ("react", "event"):
            logger.info(f"Director: {self.cast[live.speaker].name} sets about {doing}")
            act: dict[str, Any] = {"type": "act", "who": [live.speaker], "action": doing}
            # A flower in the color they said they'd pick, if they said one.
            color = flower_color(line.text) if doing == "flower" else None
            if color is not None:
                act["color"] = color
            await self.emit(act)
        mood = top(reading.mood, MOOD_FLOOR)
        gesture = top(reading.gesture, GESTURE_FLOOR)
        if mood or gesture:
            await self.emit(
                {
                    "type": "emote",
                    "who": live.speaker,
                    "mood": mood,
                    "gesture": gesture,
                    "strength": round(reading.mood.get(mood or "", 0.6), 3),
                    "role": "speak",
                    "target": live.plan.to,
                    "take": live.take,
                }
            )
        taken = top(reading.taken, MOOD_FLOOR)
        listeners = sorted(c for c in self.cast if c != live.speaker and line.heard(c))
        if taken and listeners:
            await self.emit(
                {
                    "type": "emote",
                    "who": listeners,
                    "mood": taken,
                    "strength": round(reading.taken[taken] * LISTENER_MOOD_STRENGTH, 3),
                    "role": "listen",
                    "target": live.speaker,
                }
            )

    async def _unheard(self, live: Live) -> None:
        """A take that won't be heard (it said nothing, or failed): drop it, and let whatever
        was waiting on it go on without it."""
        epoch = self._epoch
        await self.floor.drop(live.take)
        if live.epoch != epoch:
            return
        await live.confirmed.wait()
        if epoch != self._epoch:
            return
        if live.chorus is not None:
            # Whoever of the chorus is last to be written decides what follows.
            rest = [lv for lv in self._lives.values() if lv.chorus == live.chorus]
            if rest and all(lv.line is not None for lv in rest):
                self.spawn(self._follow(rest[-1]), f"follow {rest[-1].take}")
        elif self._queue:  # the rest of a group goes on without them, after the one before
            after = {live.prev.take} if live.prev is not None else set()
            await self.ask(self._queue.pop(0), after=after, prev=live.prev)
        self._maybe_quiet()

    async def _follow(self, live: Live) -> None:
        """What follows a line, decided while it waits for its turn or plays. Every await is a
        chance for the user to have taken the floor, so the epoch is checked again right before
        each take is asked."""
        await live.confirmed.wait()
        line, epoch = live.line, self._epoch
        assert line is not None
        if live.epoch != epoch or live.plan.reason == "react" or live.take not in self._lives:
            return
        if self._queue:
            await self._ahead(live)
            if epoch == self._epoch and self._queue:
                await self.ask(self._queue.pop(0), after={live.take}, prev=live)
            return
        after = {live.take}
        if live.chorus is not None:
            together = [lv for lv in self._lives.values() if lv.chorus == live.chorus]
            if live.chorus in self._decided or any(lv.line is None for lv in together):
                return  # the last of the chorus to be written decides, once
            self._decided.add(live.chorus)
            after = {lv.take for lv in together}
        if live.chorus is None:
            reading = await self.read(line)
        else:
            # Every line of the chorus is read; the one most likely to get an answer is followed.
            chorus = [ln for ln in self.transcript.lines if ln.chorus == live.chorus]
            reading = strongest(await asyncio.gather(*(self.read(ln) for ln in chorus)))
        await self._ahead(live)
        if epoch != self._epoch or line not in self.transcript.lines:
            return  # the user has spoken since: their turn wins
        nxt = self.engine.after(reading, line.heard_by)
        self.log_reading(reading, nxt)
        await self.emit(self.next_message(reading, nxt, live.take))
        reaction = nxt.reaction
        if reaction is not None and reaction.speaker not in self._speaking():
            if epoch != self._epoch:
                return
            # As the line ends, not a beat after it.
            await self.ask(reaction, after={live.take}, gap=0.0, prev=live)
        if nxt.take is not None and epoch == self._epoch:
            await self.ask(nxt.take, after=after, prev=live)

    @staticmethod
    async def _ahead(live: Live) -> None:
        """Wait until what follows `live` may be asked for: once the line before it has started
        playing. So the table is at most two lines ahead of what's being heard (one waiting its
        turn, one being written): enough that a short line isn't followed by a gap while the
        next is written, and no more is thrown away when the user cuts in."""
        if live.prev is not None:
            await live.prev.heard.wait()

    async def read(self, line: Line) -> Reading:
        """Jev's reading of a character's line: who answers it, its momentum, who reacts."""
        history = self.transcript.lines[: self.transcript.lines.index(line)]
        return await self.referee.reply(self.transcript, history, line.speaker, line.text)

    def _speaking(self) -> set[str]:
        """Characters with a take still to come or playing."""
        return {lv.speaker for lv in self._lives.values()}

    # --- What the floor reports ----------------------------------------------------------------

    async def line_started(self, take: int) -> None:
        live = self._lives.get(take)
        if live is not None:
            live.started = True
            live.heard.set()
            if live.line is not None:
                await self.emit_line(f"t{take}", live.line)
            await self._show(live)

    async def line_finished(self, take: int, heard: str | None) -> None:
        live = self._lives.pop(take, None)
        if live is not None:
            live.heard.set()
            if live.task is not None and not live.task.done():
                live.task.cancel()  # cut short: the character stops writing and voicing it
        if live is not None and heard is None and not live.started:
            # Finished without a sound (its voice failed): it was never heard, so it's taken
            # back, with anything that was to follow it.
            await self._taken_back(live)
        elif live is not None and heard is not None:
            # Cut short: recorded as far as it was heard (even if it was never all written).
            if live.line is None and heard:
                live.line = self.transcript.add(live.speaker, heard, how=live.plan.how)
            if live.line is not None and heard:
                live.line.text, live.line.interrupted = heard, True
                await self.emit_line(f"t{take}", live.line)
            else:
                if live.line is not None:
                    self.transcript.remove(live.line)
                await self.emit({"type": "line", "id": f"t{take}", "removed": True})
        elif live is not None and live.line is None:
            # Played out but never written down (the user had taken the floor by then).
            await self.emit({"type": "line", "id": f"t{take}", "removed": True})
        self._maybe_quiet()

    async def line_dropped(self, take: int) -> None:
        live = self._lives.pop(take, None)
        if live is not None:
            await self._taken_back(live)
        self._maybe_quiet()

    async def _taken_back(self, live: Live) -> None:
        """A line never heard: out of the transcript, off the client, its character stopped,
        and whatever was to follow it (a reply to it, say) dropped too."""
        live.heard.set()
        live.confirmed.set()
        if live.task is not None and not live.task.done():
            live.task.cancel()
        if live.line is not None:
            self.transcript.remove(live.line)
        # The client stops showing them as thinking.
        await self.emit({"type": "line", "id": f"t{live.take}", "removed": True})
        for after in [lv for lv in self._lives.values() if lv.prev is live]:
            await self.floor.drop(after.take)

    async def voices(self, speakers: list[str]) -> None:
        await self.emit({"type": "voices", "speakers": speakers, "at": time.time()})

    def _maybe_quiet(self) -> None:
        if not self.busy:
            self._quiet_since = time.monotonic()
            self.spawn(self._arm_lull(), "lull timer")
            if self._events:
                self.spawn(self._news(), "news")

    # --- The user ------------------------------------------------------------------------------

    async def take_floor(self) -> None:
        """The user (or the host's buttons) takes the floor: every take still to come is void,
        and every character still writing or voicing is stopped. The voices themselves are
        stopped by the floor, when the interruption reaches it."""
        self._epoch += 1
        self._wake.set()
        self._queue.clear()
        for live in list(self._lives.values()):
            if live.task is not None and not live.task.done():
                live.task.cancel()  # cancels the job: the character stops writing and voicing

    async def interrupted(self) -> None:
        """The floor is about to cut every line (the user spoke or typed over the table)."""
        await self.take_floor()

    async def user_started(self) -> None:
        self._turn_began = time.monotonic()
        self._user_speaking = True
        await self.take_floor()

    async def user_stopped(self) -> None:
        self._user_speaking = False

    async def user_turn(self, context: LLMContext) -> None:
        """A user turn has ended (Flux's call): record it, ask Jev about it, and play what
        the engine plans.

        If Jev, reading along while the user spoke, was sure who they were talking to, that
        character's line is started at once and held at the floor until the final read agrees
        (if it doesn't, it's dropped unheard). A turn that continues one nobody has answered yet
        is read as one turn.
        """
        messages = context.get_messages()
        fresh = messages[self._seen :]
        self._seen = len(messages)
        self._turns += 1
        said = normalize(" ".join(user_text(m) for m in fresh))
        guess, self._guess = self._guess, None
        self._finals.clear()
        self._interim = ""
        self._preview_want = None
        # Typed turns interrupt without the user ever starting to speak.
        await self.take_floor()
        epoch = self._epoch
        if self._lives:
            await self.floor.cut_all()
        if not said:
            return

        # When the words of this turn ended: the voice's end, unless the VAD missed them (a short
        # word, quietly said) and its last end is from before this turn: then, now.
        now = time.monotonic()
        ended = self._voice_ended if self._voice_ended >= self._turn_began else now
        line, line_id, history, merged = self._user_line(said, ended)
        await self.emit_line(line_id, line)
        early = None if merged else await self._speculate(guess)
        try:
            await self._route(epoch, line, history, ended, guess, early)
        finally:
            if early is not None and not early.lives[0].confirmed.is_set():
                # Never confirmed (a new turn took the floor, or this one failed): the lines
                # started early are dropped, and their hold with them.
                for live in early.lives:
                    await self.floor.drop(live.take)
                await self.floor.drop(early.hold)

    async def _route(
        self,
        epoch: int,
        line: Line,
        history: list[Line],
        ended: float,
        guess: Reading | None,
        early: Early | None,
    ) -> None:
        """Jev's final read of the user's turn, and what the table does about it."""
        engine = self.engine
        try:
            reading = await asyncio.wait_for(
                self.referee.addressee(
                    self.transcript, history, line.text, last_addressed=engine.addressed
                ),
                ROUTE_WAIT_S,
            )
        except TimeoutError:
            error = f"no answer in {ROUTE_WAIT_S} s"
            reading = Reading("route", USER, line.text, None, error=error)
        if epoch != self._epoch:
            return  # another turn has started since (typed, say): it routes itself
        if reading.error and early is not None and guess is not None:
            reading = guess  # no final read: the read-along's guess stands
        reading = weigh(
            reading, self._favoured({c for c in self.cast if line.heard(c)}), RECENCY_WEIGHT
        )
        # Nobody talks over the user: if they've gone on, their next words take the floor, and
        # the two are read as one turn.
        if await self._more(epoch, ended):
            return
        if not self._voice:
            await self.floor.resume()  # their words are a turn now: lines may start
        plan = engine.route(reading)
        if plan.why in ("addressed", "group", "chorus"):
            self._talked_with(t.speaker for t in plan.takes if line.heard(t.speaker))
        move = self._with_move(plan, reading)
        act = self._with_act(plan, reading)
        # Who answers, as planned: a "hands up if…" is only those it's true of.
        members = [t.speaker for t in plan.takes]
        await self.emit({**reading.to_message(), "plan": plan.why, "members": members})
        await self._listening(reading, line)
        if move is not None:
            await self.emit(move)
        if act is not None:
            await self.emit(act)
        self.log_reading(reading)
        logger.info(f"Director: the user's turn: {plan.why} {members}")
        await self._arm_lull()
        if epoch != self._epoch:
            return
        if early is not None:
            takes = ", ".join(str(lv.take) for lv in early.lives)
            if same_plan(early.plan, plan):
                logger.debug(f"Director: takes {takes} started early, and they stand")
                for live in early.lives:
                    live.confirmed.set()
                await self.floor.release(early.hold)
                return
            logger.info(f"Director: takes {takes} started early, but Jev planned otherwise")
            self._queue.clear()
            for live in early.lives:
                await self.floor.drop(live.take)
            await self.floor.release(early.hold)
        if not plan.takes and plan.why != "hush":
            self.spawn(
                self._lull_soon(), "lull after silence"
            )  # nobody answers: nothing to idle on
        await self.play(plan)

    def _user_line(self, said: str, ended: float) -> tuple[Line, str, list[Line], bool]:
        """Add what the user said to the transcript, or, if it carries on their last turn (it
        began within MERGE_GAP_S of that one's end, and nothing has been heard since), add it
        to that line. Returns the line, its id for the client, the conversation before it, and
        whether it was merged."""
        lines, last, now = self.transcript.lines, self._last_user, time.monotonic()
        # When they first spoke again after their last turn's words ended (if they have).
        again = next((t for t in self._voice_starts if t > self._last_user_end), None)
        if again is not None:
            went_on = again - self._last_user_end < MERGE_GAP_S
        else:  # no new words since: the transcript of the same words, finalised in two parts
            went_on = now - self._last_user_at < MERGE_GAP_S
        heard = frozenset(self.space.heard_user())
        if last is not None and last in lines and went_on:
            after = lines[lines.index(last) + 1 :]
            if all(ln.speaker == NOTE for ln in after):
                for note in after:  # the asides of a route that's void now
                    self.transcript.remove(note)
                last.text = normalize(f"{last.text} {said}")
                last.heard_by = (last.heard_by or frozenset()) | heard
                self._last_user_end, self._last_user_at = ended, now
                logger.info("Director: the user carried on their last turn")
                logger.debug(f'Director: the turn is now "{last.text}"')
                return last, self._last_user_id, lines[: lines.index(last)], True
        history = list(lines)
        line = self.transcript.add(USER, said, heard_by=heard)
        self._last_user, self._last_user_id = line, f"u{next(self._users)}"
        self._last_user_end, self._last_user_at = ended, now
        return line, self._last_user_id, history, False

    async def _more(self, epoch: int, ended: float) -> bool:
        """If the user's voice has started again since their turn's words `ended`, wait until
        those words become a turn (the transcript trails the voice), MORE_MAX_S at most, or
        TRANSCRIPT_LAG_S after the voice stops without one. True if a new turn has taken the
        floor."""
        start = time.monotonic()
        while epoch == self._epoch:
            if self._voice_began <= ended:  # not speaking again
                return False
            lag = self._voice_ended + TRANSCRIPT_LAG_S
            until = start + MORE_MAX_S if self._voice else min(start + MORE_MAX_S, lag)
            left = until - time.monotonic()
            if left <= 0:
                return False
            self._wake.clear()
            try:
                await asyncio.wait_for(self._wake.wait(), left)
            except TimeoutError:
                pass
        return True

    async def voice(self, on: bool) -> None:
        """The VAD heard the user's voice start or stop. Nobody starts a line while they're
        speaking: the floor pauses, and resumes when their words have become a turn (when it's
        routed), or TRANSCRIPT_LAG_S after the voice stops without one (a cough, a laugh)."""
        now = time.monotonic()
        logger.debug(f"Director: voice {'on' if on else 'off'}")
        if on and not self._voice:
            self._voice_began = now
            self._voice_starts.append(now)
        elif not on and self._voice:
            self._voice_ended = now
        if on and not self._voice and not self._user_speaking:
            # A new utterance, with no turn open: whatever was heard before it (a laugh that
            # didn't take the floor) is not part of what comes next.
            self._finals.clear()
            self._interim = ""
            self._guess = None
        self._voice = on
        self._wake.set()
        if self._resume is not None:
            self._resume.cancel()
            self._resume = None
        if on:
            await self.floor.pause()
        else:
            self._resume = self.spawn(self._resume_after(TRANSCRIPT_LAG_S), "resume")

    async def _resume_after(self, delay: float) -> None:
        await asyncio.sleep(delay)
        if not self._voice:
            await self.floor.resume()

    async def _speculate(self, guess: Reading | None) -> Early | None:
        """If Jev's read-along was sure who the user was talking to (a character, or the whole
        table), start what it plans now, held at the floor until the final read agrees."""
        if guess is None or guess.error:
            return None
        guess = weigh(guess, self._favoured(), RECENCY_WEIGHT)
        if guess.choice is None or guess.p(guess.choice) < SPECULATE_FLOOR:
            return None
        if guess.wants(INTENT_FLOOR) != ANSWER:
            return None
        plan = self.engine.plan(guess)
        if plan.why not in ("addressed", "chorus", "group") or not plan.takes:
            return None
        self._with_move(plan, guess)
        self._with_act(plan, guess)
        hold = next(self._takes)
        await self.floor.hold(hold)
        # No beat after the hold: it stands in for the final read, not for a line.
        lives = await self.play(plan, after=frozenset({hold}), gap=0.0, early=True)
        return Early(plan, lives, hold)

    async def heard(self, text: str, final: bool) -> None:
        """Words still being spoken: Jev reads them now, so the final read is ready in time."""
        if final:
            self._finals.append(text)
            self._interim = ""
        else:
            self._interim = text
        so_far = normalize(" ".join([*self._finals, self._interim]))
        if not so_far:
            return
        self._preview_want = so_far
        if self._preview_task is None or self._preview_task.done():
            self._preview_task = self.spawn(self._preview(), "preview")

    async def _preview(self) -> None:
        # One read in flight at a time; when it lands, read the latest words if they changed.
        read: str | None = None
        engine = self.engine
        while self._preview_want and self._preview_want != read:
            read, turn = self._preview_want, self._turns
            history = list(self.transcript.lines)
            reading = await self.referee.addressee(
                self.transcript, history, read, last_addressed=engine.addressed, kind="preview"
            )
            if turn != self._turns:  # the turn ended while Jev read it: the route has it
                return
            if not reading.error:
                self._guess = reading
                # Those listening react as the words come.
                await self._listening(reading)
            await self.emit(weigh(reading, self._favoured(), RECENCY_WEIGHT).to_message())

    # --- Silence, and the host's buttons -------------------------------------------------------

    async def idle(self) -> None:
        """The user aggregator says the user has been quiet a while: someone may pick something
        up, if the table isn't talking and the engine allows."""
        if self.busy or self._user_speaking or self.engine.lull_wait() is None:
            return
        if not self.space.friends_near_user():
            return  # nobody near the user who knows them to say anything
        epoch = self._epoch
        quiet = time.monotonic() - self._quiet_since
        reading = await self.referee.lull(self.transcript, quiet)
        if epoch != self._epoch or self.busy or self._user_speaking:
            return
        plan = self.engine.lull(reading, sorted(self.space.friends_near_user()))
        await self.emit({**reading.to_message(), "plan": plan.why})
        self.log_reading(reading)
        await self._arm_lull()
        await self.play(plan)

    def _dance_music(self) -> bool:
        """Whether there's music to dance to: a record on the gramophone in the hall. (Before
        the game says, there's none.)"""
        return self.space.music == "hall"

    def _favoured(self, heard: set[str] | None = None) -> str | None:
        """Whoever the user spoke to last, favoured on a near tie, as long as they can still hear
        the user (`heard`: who heard their line; by default, who hears them now). Once the user
        has walked away from them, they're no likelier to be meant than anyone."""
        favoured = self.engine.favoured
        heard = self.space.heard_user() if heard is None else heard
        return None if favoured in self.cast and favoured not in heard else favoured

    def _asked(self, plan: Plan, reading: Reading) -> list[str]:
        """Everyone a request is for: those answering it, and anyone else who heard it whom Jev
        is sure was asked too ("you two, follow me", with only one of them answering)."""
        asked = {t.speaker for t in plan.takes}
        heard = self.space.heard_user()
        asked |= {
            c
            for c, p in reading.included.items()
            if c in self.cast and c in heard and p >= ALSO_ASKED_FLOOR
        }
        return sorted(asked)

    def _with_move(self, plan: Plan, reading: Reading) -> dict[str, Any] | None:
        """If the user asked whoever they're talking to to move, those of them who heard it
        are told so in their notes, and the game is to move them: the `move` message."""
        wants = reading.wants_move(MOVE_FLOOR) if not reading.error else None
        if wants is None or plan.why not in ("addressed", "group", "chorus") or not plan.takes:
            return None
        action, area = wants
        if action == "go" and (not area or area not in self.space.areas):
            return None
        if reading.wants_act(ACT_FLOOR) == "introduce":
            return None  # they lead the way to whoever the user's to meet (`_with_act`)
        movers = self._asked(plan, reading)
        for take in plan.takes:
            home = self.space.area_name(self.cast[take.speaker].home)
            text = NOTE_MOVE[GO if action == "go" else action].format(
                home=home, area=self.space.area_name(area or "")
            )
            take.note = f"{take.note} {text}" if take.note else text
        if action == HOME:
            area = None
        return {"type": "move", "who": movers, "action": action, "area": area}

    def _with_act(self, plan: Plan, reading: Reading) -> dict[str, Any] | None:
        """If the user asked whoever they're talking to to do something (dance, play the
        piano, bring them cake), those who'll do it are told so in their notes, and the game has
        them do it: the `act` message. Only one does what only one can (bake the cake, say)."""
        action = reading.wants_act(ACT_FLOOR) if not reading.error else None
        if action is None or plan.why not in ("addressed", "group", "chorus") or not plan.takes:
            return None
        if action == "dance" and not self._dance_music():
            # Nobody dances to nothing: they ask for a record on first.
            for take in plan.takes:
                take.note = f"{take.note} {NOTE_NO_MUSIC}" if take.note else NOTE_NO_MUSIC
            return None
        doers = [plan.takes[0]] if action in SINGULAR_ACTS else plan.takes
        # Something brought or handed over is for the user, unless it's for someone else.
        to = reading.gives_to(FOR_FLOOR) if action in NOTE_ACT_FOR else None
        if to is not None and (to not in self.cast or to == doers[0].speaker):
            to = None
        if action == "introduce" and to is None:
            # Introduced to whom? They ask.
            take = doers[0]
            take.note = f"{take.note} {NOTE_INTRODUCE_WHOM}" if take.note else NOTE_INTRODUCE_WHOM
            return None
        # A flower in the color the user asked for, if they did: otherwise, whatever's picked.
        color = flower_color(reading.heard) if action == "flower" else None
        for take in doers:
            take.act = action
            if color is not None:
                whom = self.cast[to].name if to is not None else "the person"
                text = NOTE_FLOWER_COLOR.format(to=whom, color=color)
            elif action == "introduce" and to in self.space.listeners(take.speaker):
                text = NOTE_INTRODUCE_HERE.format(to=self.cast[to].name)
            elif to is not None:
                text = NOTE_ACT_FOR[action].format(to=self.cast[to].name)
            else:
                text = NOTE_ACT[action]
            take.note = f"{take.note} {text}" if take.note else text
        who = [plan.takes[0].speaker] if action in SINGULAR_ACTS else self._asked(plan, reading)
        act: dict[str, Any] = {"type": "act", "who": who, "action": action}
        if to is not None:
            act["to"] = to
        if color is not None:
            act["color"] = color
        return act

    async def _listening(self, reading: Reading, line: Line | None = None) -> None:
        """How those who heard the user take what they're saying, as they listen."""
        mood = top(reading.feel, MOOD_FLOOR) if not reading.error else None
        heard = self.space.heard_user() if line is None else {c for c in self.cast if line.heard(c)}
        if mood is None or not heard:
            return
        await self.emit(
            {
                "type": "emote",
                "who": sorted(heard),
                "mood": mood,
                "strength": round(reading.feel[mood] * LISTENER_MOOD_STRENGTH, 3),
                "role": "listen",
                "target": USER,
            }
        )

    async def _lull_soon(self) -> None:
        """A turn nobody answered out loud leaves the user aggregator's idle timer unarmed (it
        starts when the table stops talking): so wait out the lull here instead."""
        wait = self.engine.lull_wait()
        if wait is not None:
            await asyncio.sleep(wait)
            await self.idle()

    async def _arm_lull(self) -> None:
        """Set the user aggregator's idle timer to the engine's next wait (0: off)."""
        wait = self.engine.lull_wait() or 0.0
        if self.worker is None or wait == self._idle_timeout:
            return
        self._idle_timeout = wait
        await self.worker.queue_frame(UserIdleTimeoutUpdateFrame(timeout=wait))

    async def host(self, action: str) -> None:
        """The client's buttons: "hush" (everyone stops, and the table stays quiet until the
        user speaks) or "carry_on" (the table picks up where it was)."""
        logger.info(f"Director: the host says {action}")
        if action == "hush":
            await self.take_floor()
            await self.floor.cut_all()
            self.engine.hush()
            await self._arm_lull()
            await self.emit({"type": "host", "action": action, "at": time.time()})
        elif action == "carry_on" and not self.busy:
            await self.emit({"type": "host", "action": action, "at": time.time()})
            await self.play(self.engine.carry_on())
            await self._arm_lull()

    # --- Reporting -----------------------------------------------------------------------------

    async def emit_line(self, id_: str, line: Line) -> None:
        await self.emit(
            {
                "type": "line",
                "id": id_,
                "speaker": line.speaker,
                "text": line.text,
                "how": line.how,
                "interrupted": line.interrupted,
                "heard_by": None if line.heard_by is None else sorted(line.heard_by),
                "at": time.time(),
            }
        )

    @staticmethod
    def next_message(reading: Reading, nxt: Next, take: int) -> dict[str, Any]:
        return {
            **reading.to_message(),
            "take": take,
            "next": nxt.take.speaker if nxt.take else USER,
            "why": nxt.why,
            "energy": round(nxt.energy, 3),
            "drive": None if nxt.drive is None else round(nxt.drive, 3),
            "reaction": nxt.reaction.speaker if nxt.reaction else None,
        }

    def log_reading(self, reading: Reading, nxt: Next | None = None) -> None:
        top = sorted(reading.probabilities.items(), key=lambda kv: -kv[1])[:3]
        odds = ", ".join(f"{k} {v:.2f}" for k, v in top)
        how = "cached" if reading.cached else f"{reading.ms:.0f} ms"
        error = f" ({reading.error})" if reading.error else ""
        extra = ""
        if reading.choice == GROUP:
            asked = ", ".join(
                f"{c} {reading.included[c]:.2f}" for c in reading.group(list(self.cast))
            )
            extra += f"; asked: {asked}; chorus {reading.chorus or 0:.2f}"
        if reading.intent:
            extra += "; intent " + ", ".join(f"{k} {v:.2f}" for k, v in reading.intent.items())
        if reading.raw:
            extra += f" (×{reading.weight:g} for {reading.favoured})"
        if nxt is not None:
            extra += (
                f"; momentum {reading.momentum or 0:.2f} × energy {nxt.energy:.2f}"
                f" → {nxt.take.speaker if nxt.take else 'user'} ({nxt.why})"
            )
            if nxt.reaction:
                extra += f", {nxt.reaction.speaker} reacts"
        # What was said is only logged at DEBUG: production logs hold the decisions, not the words.
        logger.info(f"Jev {reading.kind}: {odds}{extra} [{how}]{error}")
        logger.debug(f'Jev {reading.kind} read: "{reading.heard}"')

    # --- Processors ----------------------------------------------------------------------------

    def hearing(self) -> Hearing:
        return Hearing(self)

    def router(self) -> Router:
        return Router(self)


class Hearing(FrameProcessor):
    """Between the STT and the user aggregator: the user's words as they are heard."""

    def __init__(self, director: Director) -> None:
        super().__init__(name="Hearing")
        self._director = director

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        if (
            isinstance(frame, (TranscriptionFrame, InterimTranscriptionFrame))
            and frame.text.strip()
        ):
            await self._director.heard(frame.text, isinstance(frame, TranscriptionFrame))
        elif isinstance(frame, (VADUserStartedSpeakingFrame, VADUserStoppedSpeakingFrame)):
            # The user aggregator runs the VAD, and always sends its verdicts back up this
            # way (downstream, only some of the time).
            if direction == FrameDirection.UPSTREAM:
                await self._director.voice(isinstance(frame, VADUserStartedSpeakingFrame))
        await self.push_frame(frame, direction)


class Router(FrameProcessor):
    """After the user aggregator: each finished user turn goes to the director."""

    def __init__(self, director: Director) -> None:
        super().__init__(name="Router")
        self._director = director
        self._route: asyncio.Task | None = None

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        director = self._director
        if isinstance(frame, LLMContextFrame) and direction == FrameDirection.DOWNSTREAM:
            if frame.speculation:
                logger.warning("Router: a speculative turn isn't routed (eager turn-taking is off)")
                return
            # Routed off the frame loop: Jev takes a few hundred ms, and speech that starts in
            # the meantime cancels the route (the new words make a turn of their own).
            self._route = director.spawn(director.user_turn(frame.context), "route")
            return
        if isinstance(frame, UserStartedSpeakingFrame):
            if self._route is not None and not self._route.done():
                self._route.cancel()
            await director.user_started()
        elif isinstance(frame, UserStoppedSpeakingFrame):
            await director.user_stopped()
        await self.push_frame(frame, direction)
