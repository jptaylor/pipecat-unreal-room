"""The floor: whose voice plays when, in the room worker's pipeline.

    … → Router → CastBridge → FloorGate → transport.output() → FloorEar

Every character has their own TTS and their own transport destination (a channel of the game's
audio, `mixer.py`, which plays it from their head), so their voices can overlap. The floor decides
when each line may play:

- `CastBridge` takes the characters' voices off the bus (their TTS frames, each stamped with the
  take, the line it belongs to). Nothing in the room crosses back: characters get lines as jobs.
- `FloorGate` holds each line until its turn. A line is *expected* (`Floor.expect`) with the
  lines it must wait for: a reply waits for the line it answers, plus a beat; a chorus waits for
  nothing, its voices staggered a little; a reaction waits for the line it reacts to and comes in
  as it ends. Held frames are released together, their word timings moved along by however long
  they were held. When the user cuts in, the gate drops every held line and interrupts every
  destination (an interruption only clears the destination it names), so all the voices stop.
  The output transport says when each destination starts and stops speaking; the gate passes on
  one "bot speaking" for the room (from the first voice starting to the last one stopping), so
  the user's turn-taking and idle timer see the table as one speaker.
- `FloorEar`, after the output, keeps the words actually heard of each line, so a line cut short
  is recorded as far as it was heard.

The floor reports to its listener (the director) as each line starts, finishes (or is cut short),
or is dropped unheard, and whenever the set of voices playing changes.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Protocol

from loguru import logger
from pipecat.bus import BusBridgeProcessor
from pipecat.bus.messages import BusFrameMessage, BusMessage
from pipecat.frames.frames import (
    BotStartedSpeakingFrame,
    BotStoppedSpeakingFrame,
    ErrorFrame,
    Frame,
    InterruptionFrame,
    TTSAudioRawFrame,
    TTSStartedFrame,
    TTSStoppedFrame,
    TTSTextFrame,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from cast import take_of
from config import FINISH_GRACE_S
from room import normalize

VOICE_FRAMES = (TTSStartedFrame, TTSAudioRawFrame, TTSTextFrame, TTSStoppedFrame)

WAITING, OPEN, FINISHED, DROPPED = "waiting", "open", "finished", "dropped"
RELEASE_SLACK_S = 0.005  # a line due this soon is opened now
HOLD = ""  # the speaker of a hold: a cue with no voice, that the lines after it wait for
PRUNE_AFTER_S = 30.0  # lines finished this long ago, that nothing waits on, are forgotten


class Listener(Protocol):
    async def interrupted(self) -> None:
        """The user cut in (told before any line is cut)."""

    async def line_started(self, take: int) -> None: ...

    async def line_finished(self, take: int, heard: str | None) -> None:
        """`heard` is None for a whole line, or what was heard of a line cut short."""

    async def line_dropped(self, take: int) -> None: ...

    async def voices(self, speakers: list[str]) -> None: ...


@dataclass(eq=False)
class Cue:
    """One line's place on the floor."""

    take: int
    speaker: str
    after: set[int]
    gap: float  # seconds after the last of `after` finishes
    not_before: float  # monotonic time it may start, at the earliest
    chorus: int | None = None  # one of several lines said at once
    slot: float | None = None  # ...starting this long after the chorus's first voice is ready
    state: str = WAITING
    held: list[Frame] = field(default_factory=list)
    audio_at: int | None = None  # pipeline clock time its audio started going out
    shift: int | None = None  # how far its word timings move (ns)
    started: bool = False  # its audio has been heard
    sounding: bool = False  # ...and is playing now
    complete: bool = False  # the character has sent all of it...
    stops: int = 0  # ...ending this many TTS runs
    stops_seen: int = 0  # the ends of TTS runs gone out so far
    sealed: bool = False  # dropped while going out: nothing more of it is taken
    progress_at: float = 0.0  # when a frame of it last went out, or its voice started or stopped
    finished_at: float | None = None
    gone_at: float | None = None  # when it was finished or dropped (for pruning)
    words: list[str] = field(default_factory=list)
    timer: asyncio.Task | None = None  # opens it when its turn comes
    watch: asyncio.Task | None = None  # finishes it if what it waits for never comes

    @property
    def live(self) -> bool:
        """Still to come, or playing."""
        return self.state in (WAITING, OPEN)


class Floor:
    """The cues, shared by the gate (before the output) and the ear (after it)."""

    def __init__(self, listener: Listener, cast: Sequence[str]) -> None:
        self._listener = listener
        self._cast = list(cast)
        self._cues: dict[int, Cue] = {}
        self._sounding: set[str] = set()
        self._chorus_start: dict[int, float] = {}  # when each chorus's first voice was ready
        self._slots: dict[int, list[float]] = {}  # each chorus's stagger, handed out in order
        self._paused = False  # the user's voice is active: no line starts
        self._lock = asyncio.Lock()
        self._events: list[tuple[str, int | list[str], str | None]] = []
        self._gate = FloorGate(self)
        self._ear = FloorEar(self)

    def gate(self) -> FloorGate:
        return self._gate

    def ear(self) -> FloorEar:
        return self._ear

    # --- The director's side --------------------------------------------------------------------

    async def expect(
        self,
        take: int,
        speaker: str,
        *,
        after: set[int],
        gap: float = 0.0,
        delay: float = 0.0,
        chorus: int | None = None,
    ) -> None:
        """A line is coming from `speaker`: it plays once every line in `after` has finished
        (or been dropped), `gap` seconds after the last of them, and `delay` from now at the
        earliest. In a chorus, `delay` is one of the chorus's stagger slots instead: they're
        handed out in the order the voices are ready, so the first ready goes first. A line
        also waits for `speaker`'s own line before it, if one is still going."""
        async with self._lock:
            own = set()
            if speaker != HOLD:
                own = {c.take for c in self._cues.values() if c.speaker == speaker and c.live}
            not_before = time.monotonic() + (0.0 if chorus is not None else delay)
            cue = Cue(take, speaker, set(after) | own, gap, not_before, chorus)
            cue.progress_at = time.monotonic()
            if chorus is not None:
                self._slots.setdefault(chorus, []).append(delay)
            self._cues[take] = cue
            logger.debug(f"Floor: take {take} ({speaker or 'hold'}) waits for {sorted(cue.after)}")
            await self._release()
        await self._dispatch()

    async def hold(self, take: int) -> None:
        """A hold: lines expected after `take` wait until it's released (or dropped)."""
        await self.expect(take, HOLD, after=set())

    async def release(self, take: int) -> None:
        """Release a hold: the lines waiting on it play as soon as they're ready."""
        await self.complete(take, stops=0)

    async def complete(self, take: int, stops: int) -> None:
        """The character has sent every frame of this line: `stops` runs of TTS audio, each
        ending with a TTSStoppedFrame (0: it had nothing to say). It's finished once those have
        all gone out and played, not before: frames and this word take different paths."""
        async with self._lock:
            cue = self._cues.get(take)
            if cue is not None and cue.live:
                cue.complete, cue.stops, cue.progress_at = True, stops, time.monotonic()
                logger.debug(f"Floor: take {take} complete, {stops} runs ({cue.state})")
                if cue.chorus is not None and cue.slot is None:
                    # A voice of a chorus that never sounded (its TTS failed, say): it takes a
                    # slot anyway, so it's opened, and finishes, and doesn't hold up the rest.
                    self._take_slot(cue)
                    await self._release()
                await self._settle(cue)
        await self._dispatch()

    def _take_slot(self, cue: Cue) -> None:
        """The earliest slot of its chorus still free, for a voice that's ready (or done)."""
        assert cue.chorus is not None
        slots = sorted(self._slots.get(cue.chorus) or [0.0])
        cue.slot = slots.pop(0)
        self._slots[cue.chorus] = slots
        self._chorus_start.setdefault(cue.chorus, time.monotonic())

    async def drop(self, take: int) -> None:
        """Abandon a line. Unheard, it's dropped; already going out, it's left to finish as it
        is (nothing more of it is taken), rather than interrupted: an interruption flushes every
        processor after the gate, and other voices' frames with it."""
        async with self._lock:
            cue = self._cues.get(take)
            if cue is not None and cue.live:
                if cue.state == OPEN and cue.audio_at is not None:
                    cue.sealed, cue.complete, cue.stops = True, True, cue.stops_seen
                    await self._settle(cue)
                else:
                    await self._cut(cue)
                await self._release()
        await self._dispatch()

    async def pause(self) -> None:
        """The user is speaking: no line starts until `resume`. Lines already playing go on (a
        couple of words from the user cut them off); lines not yet heard go back to waiting."""
        async with self._lock:
            self._paused = True
            for cue in self._cues.values():
                if cue.state == OPEN and cue.audio_at is None and cue.speaker != HOLD:
                    cue.state = WAITING
        await self._dispatch()

    async def resume(self) -> None:
        """Lines may start again; a chorus none of whose voices has been heard yet restarts its
        stagger from now."""
        async with self._lock:
            if not self._paused:
                return
            self._paused = False
            now = time.monotonic()
            for chorus in list(self._chorus_start):
                voices = [c for c in self._cues.values() if c.chorus == chorus and c.live]
                if voices and all(c.audio_at is None for c in voices):
                    self._chorus_start[chorus] = now
            await self._release()
        await self._dispatch()

    async def cut_all(self) -> None:
        """Every voice stops now (the host asked for quiet, or typed over the table)."""
        async with self._lock:
            await self._interrupt()
        await self._dispatch()

    # --- The gate's side ------------------------------------------------------------------------

    async def voice(self, frame: Frame) -> None:
        """A frame of a character's line, from the bridge."""
        async with self._lock:
            cue = self._cues.get(take_of(frame) or -1)
            if cue is None or not cue.live or cue.sealed:
                return  # a line abandoned, long gone, or dropped while going out
            if cue.state == WAITING:
                cue.held.append(frame)
                if cue.chorus is not None and cue.slot is None:
                    # Ready: this voice takes the chorus's earliest slot still free.
                    self._take_slot(cue)
                    await self._release()
            else:
                await self._forward(cue, frame)
                if isinstance(frame, TTSStoppedFrame):
                    await self._settle(cue)
        await self._dispatch()

    async def interrupted(self, frame: InterruptionFrame) -> None:
        """The user cut in: the director hears first (so nothing new is asked on the way), then
        every line stops, and every destination is cleared."""
        await self._listener.interrupted()
        async with self._lock:
            await self._gate.push_frame(frame)
            await self._interrupt(push_original=False)
        await self._dispatch()

    async def sounding(self, speaker: str, on: bool) -> None:
        """The output says a destination started or stopped playing."""
        async with self._lock:
            cue = self._playing(speaker)
            was = bool(self._sounding)
            if on:
                self._sounding.add(speaker)
            else:
                self._sounding.discard(speaker)
            # One speaker for the room: the first voice starting, the last one stopping.
            if not was and self._sounding:
                await self._gate.push_frame(BotStartedSpeakingFrame(), FrameDirection.UPSTREAM)
            elif was and not self._sounding:
                await self._gate.push_frame(BotStoppedSpeakingFrame(), FrameDirection.UPSTREAM)
            self._events.append(("voices", sorted(self._sounding), None))
            logger.debug(f"Floor: {speaker} {'on' if on else 'off'} (take {cue and cue.take})")
            if cue is not None:
                cue.sounding, cue.progress_at = on, time.monotonic()
                if on and not cue.started:
                    cue.started = True
                    self._events.append(("started", cue.take, None))
                if not on:
                    await self._settle(cue)
        await self._dispatch()

    def heard(self, frame: TTSTextFrame) -> None:
        """A word of a line, as it plays (the ear)."""
        cue = self._cues.get(take_of(frame) or -1)
        if cue is not None and cue.state == OPEN:
            cue.words.append(frame.text)

    # --- Inside (all with the lock held) ----------------------------------------------------------

    def _playing(self, speaker: str) -> Cue | None:
        """The line of `speaker`'s whose audio is going out."""
        return next(
            (
                c
                for c in self._cues.values()
                if c.speaker == speaker and c.state == OPEN and c.audio_at is not None
            ),
            None,
        )

    async def _forward(self, cue: Cue, frame: Frame) -> None:
        cue.progress_at = time.monotonic()
        clock = self._gate.get_clock().get_time()
        if isinstance(frame, TTSAudioRawFrame) and cue.audio_at is None:
            cue.audio_at = clock
        if isinstance(frame, TTSStoppedFrame):
            cue.stops_seen += 1
        elif frame.pts:
            # Word timings were set when the character's TTS made them: move them along by
            # however long the line was held, so they come out with the audio.
            if cue.shift is None:
                cue.shift = max(0, (cue.audio_at or clock) - frame.pts)
            frame.pts += cue.shift
        await self._gate.push_frame(frame)

    async def _release(self) -> None:
        """Open every line whose turn has come; time the ones whose turn is coming."""
        if self._paused:
            return
        now = time.monotonic()
        self._prune(now)
        for cue in sorted(self._cues.values(), key=lambda c: c.take):
            if cue.state != WAITING:
                continue
            before = [self._cues[t] for t in cue.after if t in self._cues]
            if any(c.live for c in before):
                continue
            ends = [c.finished_at for c in before if c.finished_at is not None]
            at = max([cue.not_before, *(end + cue.gap for end in ends)])
            if cue.chorus is not None:
                if cue.slot is None:
                    continue  # this voice isn't ready yet
                at = max(at, self._chorus_start[cue.chorus] + cue.slot)
            if at > now + RELEASE_SLACK_S:
                # A timer to come back then. The event loop may wake a timer a hair early, so
                # a timer that finds it isn't quite time yet sets the next one itself.
                timer = cue.timer
                if timer is None or timer.done() or timer is asyncio.current_task():
                    cue.timer = self._gate.create_task(self._release_at(at), f"floor-{cue.take}")
                continue
            cue.state, cue.progress_at = OPEN, now
            held, cue.held = cue.held, []
            logger.debug(f"Floor: take {cue.take} ({cue.speaker}) opens, {len(held)} frames held")
            for frame in held:
                await self._forward(cue, frame)
            await self._settle(cue)

    async def _release_at(self, at: float) -> None:
        await asyncio.sleep(max(0.0, at - time.monotonic()))
        async with self._lock:
            await self._release()
        await self._dispatch()

    async def _settle(self, cue: Cue) -> None:
        """Finish a line once the character has sent all of it, every run of its audio has gone
        out, and it has played (or it had no audio at all). While it waits on something that
        should come, a watchdog finishes it anyway after FINISH_GRACE_S with no progress, so a
        lost frame or a silent output never holds up whoever waits on it."""
        if cue.state != OPEN or cue.sounding:
            return
        voiced = cue.stops > 0 or cue.audio_at is not None
        if cue.complete and cue.stops_seen >= cue.stops and (cue.started or not voiced):
            cue.state, cue.finished_at, cue.gone_at = FINISHED, time.monotonic(), time.monotonic()
            logger.debug(f"Floor: take {cue.take} ({cue.speaker or 'hold'}) finished")
            self._events.append(("finished", cue.take, None))
            await self._release()
        elif (cue.complete or cue.started) and (cue.watch is None or cue.watch.done()):
            cue.watch = self._gate.create_task(self._watch(cue.take), f"floor-watch-{cue.take}")

    async def _watch(self, take: int) -> None:
        """Wait for a line to make progress (a frame going out, its voice starting or stopping);
        after FINISH_GRACE_S with none while it isn't playing, finish it as it is."""
        while True:
            cue = self._cues.get(take)
            if cue is None or cue.state != OPEN:
                return
            idle = time.monotonic() - cue.progress_at
            if cue.sounding or idle < FINISH_GRACE_S:
                await asyncio.sleep(FINISH_GRACE_S - idle if not cue.sounding else FINISH_GRACE_S)
                continue
            async with self._lock:
                if cue.state == OPEN and not cue.sounding:
                    logger.warning(
                        f"Floor: take {take} ({cue.speaker}) stalled "
                        f"({cue.stops_seen}/{cue.stops} runs out, heard: {cue.started}); moving on"
                    )
                    cue.complete, cue.stops, cue.started = True, cue.stops_seen, True
                    await self._settle(cue)
            await self._dispatch()
            return

    async def _cut(self, cue: Cue) -> None:
        for task in (cue.timer, cue.watch):
            if task is not None and task is not asyncio.current_task():
                task.cancel()
        cue.held.clear()
        cue.gone_at = time.monotonic()
        logger.debug(f"Floor: take {cue.take} ({cue.speaker or 'hold'}) cut ({cue.state})")
        if cue.started:
            cue.state, cue.finished_at = FINISHED, time.monotonic()
            self._events.append(("cut", cue.take, normalize(" ".join(cue.words))))
        else:
            cue.state = DROPPED
            self._events.append(("dropped", cue.take, None))

    async def _interrupt(self, push_original: bool = True) -> None:
        for cue in self._cues.values():
            if cue.live:
                await self._cut(cue)
        if push_original:
            await self._gate.push_frame(InterruptionFrame())
        for speaker in self._cast:
            await self._gate.push_frame(destined(InterruptionFrame(), speaker))

    def _prune(self, now: float) -> None:
        """Forget lines long gone, that nothing still waits on."""
        waited_on = {t for c in self._cues.values() if c.live for t in c.after}
        for take, cue in list(self._cues.items()):
            if cue.gone_at is not None and now - cue.gone_at > PRUNE_AFTER_S:
                if take not in waited_on:
                    del self._cues[take]
        choruses = {c.chorus for c in self._cues.values()}
        for chorus in [c for c in self._chorus_start if c not in choruses]:
            del self._chorus_start[chorus]
        for chorus in [c for c in self._slots if c not in choruses]:
            del self._slots[chorus]

    async def _dispatch(self) -> None:
        """Tell the listener what happened, outside the lock (it may call back in)."""
        events, self._events = self._events, []
        for kind, value, heard in events:
            try:
                if kind == "voices":
                    assert isinstance(value, list)
                    await self._listener.voices(value)
                    continue
                assert isinstance(value, int)
                if kind == "started":
                    await self._listener.line_started(value)
                elif kind == "finished":
                    await self._listener.line_finished(value, None)
                elif kind == "cut":
                    await self._listener.line_finished(value, heard)
                elif kind == "dropped":
                    await self._listener.line_dropped(value)
            except Exception:  # noqa: BLE001 — a listener's failure mustn't stall the floor
                logger.exception(f"Floor: the listener failed on {kind}")


def destined(frame: Frame, destination: str) -> Frame:
    frame.transport_destination = destination
    return frame


class CastBridge(BusBridgeProcessor):
    """Takes the characters' voices off the bus. Nothing crosses the other way."""

    def __init__(self, cast: Sequence[str], **kwargs) -> None:
        super().__init__(exclude_frames=(Frame,), name="CastBridge", **kwargs)
        self._cast = set(cast)

    async def on_bus_message(self, message: BusMessage) -> None:
        if not isinstance(message, BusFrameMessage) or message.source not in self._cast:
            return
        frame = message.frame
        if isinstance(frame, ErrorFrame):
            logger.warning(f"{message.source}: {frame.error}")
        elif (
            message.direction == FrameDirection.DOWNSTREAM
            and isinstance(frame, VOICE_FRAMES)
            and take_of(frame) is not None
        ):
            await self.push_frame(frame)


class FloorGate(FrameProcessor):
    """Before the output: holds each line until its turn; one "bot speaking" for the room."""

    def __init__(self, floor: Floor) -> None:
        super().__init__(name="FloorGate")
        self._floor = floor

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        floor = self._floor
        if direction == FrameDirection.UPSTREAM:
            if isinstance(frame, (BotStartedSpeakingFrame, BotStoppedSpeakingFrame)):
                if frame.transport_destination:
                    started = isinstance(frame, BotStartedSpeakingFrame)
                    await floor.sounding(frame.transport_destination, started)
                return  # the room's own, merged, was pushed by the floor
            await self.push_frame(frame, direction)
        elif isinstance(frame, InterruptionFrame) and frame.transport_destination is None:
            await floor.interrupted(frame)
        elif isinstance(frame, VOICE_FRAMES) and take_of(frame) is not None:
            await floor.voice(frame)
        else:
            await self.push_frame(frame, direction)


class FloorEar(FrameProcessor):
    """After the output: the words of each line as they're heard."""

    def __init__(self, floor: Floor) -> None:
        super().__init__(name="FloorEar")
        self._floor = floor

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        if isinstance(frame, TTSTextFrame) and direction == FrameDirection.DOWNSTREAM:
            self._floor.heard(frame)
        await self.push_frame(frame, direction)
