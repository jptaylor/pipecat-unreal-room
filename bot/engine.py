"""The engine: what the table does next, given Jev's readings. No I/O and no clock, so the room
(`director.py`, in audio) and the text harness (`scripts/converse.py`) play out the same plans.

- **The user's turn** (`route`). As host, the user can ask for an answer (one character, a group
  in turn, or a group all at once: a chorus), for the table to carry on, or for quiet.
- **After each line** (`after`). Someone answers it while the conversation has life in it. Jev
  scores each line's momentum, and the table tires as it talks: its energy halves every
  `ENERGY_HALF_LIFE_S` seconds of talk since the user last spoke. A line is answered while
  momentum × energy holds up; when it doesn't, the next speaker is asked to land it (a wrap-up),
  and then the floor is the user's. A line that has already landed (no momentum) ends it at once.
  Someone may also react out loud as the line ends.
- **Silence** (`lull`). When the user says nothing, someone picks something up, a few times,
  each exchange starting more tired than the last, until the table settles.

In the house, only those close enough hear a line (`space.py`), and only they can answer it: the
user's turn goes to whoever they're talking to among those who heard it; if they spoke to someone
too far away, someone who did hear may say so. After a line, only those who heard it answer or
react to it.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field

from config import (
    CHORUS_FLOOR,
    CONTINUE_FLOOR,
    ENERGY_HALF_LIFE_S,
    INTENT_FLOOR,
    LANDED_MOMENTUM,
    LULL_HEAD_START_S,
    LULL_WAITS_S,
    MAX_TALK_S,
    ONLY_IF_FLOOR,
    REACT_COOLDOWN,
    REACT_FLOOR,
    REPLY_FLOOR,
    TABLE_FLOOR,
    TRUE_OF_FLOOR,
)
from room import (
    CARRY_ON,
    GROUP,
    HUSH,
    NOTE,
    NOTE_CARRY_ON_CUT,
    NOTE_CARRY_ON_TABLE,
    NOTE_CHORUS,
    NOTE_CHORUS_IF,
    NOTE_GROUP_FIRST,
    NOTE_GROUP_NEXT,
    NOTE_LULL,
    NOTE_NOT_HEARD,
    NOTE_REACT,
    NOTE_REPLY,
    NOTE_SWITCH,
    NOTE_WELCOME,
    NOTE_WRAP,
    REACTION,
    SAID,
    TOGETHER,
    USER,
    Line,
    Reading,
    Transcript,
    mentions,
    names,
)


@dataclass
class Take:
    """One line for a character to say, why, the note it is given about the moment, and who
    they're saying it to (USER, a character, or nobody in particular)."""

    speaker: str
    # "welcome", "addressed", "group", "chorus", "fallback", "carry_on", "reply", "wrap",
    # "react", "lull" or "not_heard"
    reason: str
    note: str | None = None
    to: str | None = None
    act: str | None = None  # what they've been asked to do, as they say it
    moving: bool = False  # ...or asked to move: follow, come, wait or go somewhere

    @property
    def how(self) -> str:
        if self.reason in ("chorus", "welcome"):
            return TOGETHER
        return REACTION if self.reason == "react" else SAID


@dataclass
class Plan:
    """What the table does: lines in turn, in order, or all at once (a chorus)."""

    takes: list[Take] = field(default_factory=list)
    together: bool = False
    # "welcome", "meet", "addressed", "group", "chorus", "nobody", "fallback", "carry_on", "hush",
    # "lull", "not_heard" or "unheard" (nobody was close enough to hear the user)
    why: str = ""
    # A user's turn, once it's acted on: who they spoke to, and a note for the transcript that
    # every character sees ("Otto answers that; it isn't true of the others…").
    addressed: list[str] | None = None
    aside: str | None = None


@dataclass
class Next:
    """What follows a line: who answers it (if anyone), who reacts to it (if anyone), and why.

    `why` is "reply" or "wrap" when someone answers; otherwise why the floor is the user's:
    Jev chose the user ("user"), was unsure of the one it leant to ("unsure"), the line had landed
    ("landed"), the wrap-up has been said ("wrapped"), or Jev couldn't answer ("error").
    """

    take: Take | None
    reaction: Take | None
    why: str
    energy: float
    drive: float | None = None  # momentum × energy, when it decided


CARRY_ON_LOOKBACK = 6  # lines back that "carry on" looks for someone cut off


def strongest(readings: Sequence[Reading]) -> Reading:
    """After a chorus, the line most likely to get an answer from someone at the table: every
    line is read, and the one Jev is surest someone answers (rather than the user) is followed."""

    def pull(reading: Reading) -> float:
        if reading.error or reading.choice in (None, USER):
            return 0.0
        return reading.p(reading.choice or "") * (reading.momentum or 0.0)

    return max(readings, key=pull)


def group_takes(
    members: Sequence[str], transcript: Transcript, reason: str, first: str, next_: str
) -> list[Take]:
    """One line each, in order; each is told who was asked and who has answered already."""
    labels = [transcript.label(m) for m in members]
    who = everyone_or(members, transcript)
    takes = []
    for i, member in enumerate(members):
        note = first if i == 0 else next_
        done, have = names(labels[:i]), "has" if i == 1 else "have"
        takes.append(Take(member, reason, note.format(who=who, done=done, have=have), USER))
    return takes


def everyone_or(members: Sequence[str], transcript: Transcript) -> str:
    if len(members) == len(transcript.cast):
        return "everyone at the table"
    return names([transcript.label(m) for m in members])


class Engine:
    """The table's policy. `transcript` is the one conversation; the engine reads it, and keeps
    only what the transcript can't tell it: who the user spoke to last, whether a wrap-up has been
    asked for, how many silences the table has filled, and whether the user asked for quiet."""

    def __init__(self, transcript: Transcript) -> None:
        self.transcript = transcript
        # Who the user spoke to last: Jev's choice (a character, or GROUP), for the recency
        # prior, and everyone it cued, for Jev's state.
        self.favoured: str | None = None
        self.addressed: list[str] = []
        self._mark: Line | None = None  # the exchange is the talk after this line
        self._head_start = 0.0  # seconds the current exchange counts as already talked
        self._wrapping = False
        self._lulls = 0
        self._hushed = False
        self._since_reaction = REACT_COOLDOWN

    # --- The table's energy ---------------------------------------------------------------------

    def talked(self) -> float:
        """Seconds of talk in this exchange (estimated from the words): since the user last spoke,
        or since they told the table to carry on."""
        lines = self.transcript.since_user()
        if self._mark is not None and self._mark in lines:
            lines = lines[lines.index(self._mark) + 1 :]
        return Transcript.talked(lines) + self._head_start

    def _fresh(self, mark: Line | None = None) -> None:
        """A new exchange, at full energy, counting the talk after `mark` (by default, the
        last line)."""
        lines = self.transcript.lines
        self._mark = mark if mark is not None else (lines[-1] if lines else None)
        self._head_start, self._wrapping, self._lulls, self._hushed = 0.0, False, 0, False

    def _user_line(self) -> Line | None:
        return next((ln for ln in reversed(self.transcript.lines) if ln.speaker == USER), None)

    def energy(self) -> float:
        return 0.5 ** (self.talked() / ENERGY_HALF_LIFE_S)

    # --- Plans ----------------------------------------------------------------------------------

    def welcome(self) -> Plan:
        """Everyone says hi at once."""
        takes = [Take(c, "welcome", NOTE_WELCOME, USER) for c in self.transcript.cast]
        return Plan(takes, together=True, why="welcome")

    def route(self, reading: Reading) -> Plan:
        """The user has spoken (their line is already in the transcript): what they asked for,
        committed: a new exchange starts, and who they spoke to is remembered."""
        plan = self.plan(reading)
        self._fresh(self._user_line())
        if plan.why == "hush":
            self._hushed = True
        if reading.choice and plan.why not in ("hush", "carry_on"):
            self.favoured = reading.choice
        if plan.addressed is not None:
            self.addressed = plan.addressed
        if plan.aside:
            # Seen by those who heard what it's about.
            mark = self._user_line()
            self.transcript.add(NOTE, plan.aside, heard_by=mark.heard_by if mark else None)
        return plan

    def plan(self, reading: Reading) -> Plan:
        """What `route` would do with `reading`, without doing it: so a guess (Jev's read of the
        words while they were still being spoken) can be acted on before the final read lands.
        Without an answer from Jev, whoever spoke last carries on."""
        transcript, ids = self.transcript, list(self.transcript.cast)
        # Whoever spoke last before the user. (A line started early, before Jev's final read,
        # may already follow the user's line in the transcript.)
        mark = self._user_line()
        before = transcript.lines[: transcript.lines.index(mark)] if mark else transcript.lines
        last = next((ln for ln in reversed(before) if ln.by_character), None)
        # Only those close enough to hear it can answer it.
        heard = [c for c in ids if mark is None or mark.heard(c)]
        if not heard:
            return Plan(why="unheard")
        if last is not None and last.speaker not in heard:
            last = None
        wants = reading.wants(INTENT_FLOOR) if not reading.error else None
        if wants == HUSH:
            return Plan(why="hush")
        if wants == CARRY_ON and last is not None:
            # Jev's pick of who "carry on" is said to is only news if it's someone the user
            # didn't just ask something ("carry on, Theo"); otherwise the old thread resumes.
            named = reading.choice if reading.choice not in self.addressed else None
            return self._carry_on(named)
        if reading.choice == GROUP:
            asked = reading.group(ids)
            members = [m for m in asked if m in heard]
            if not members:
                return self._not_heard(reading, heard, asked)
            if len(members) == 1:
                take = Take(members[0], "addressed", None, USER)
                return Plan([take], why="addressed", addressed=members)
            if (reading.chorus or 0.0) >= CHORUS_FLOOR:
                if (reading.only_if or 0.0) >= ONLY_IF_FLOOR:
                    # "Hands up if…": only those Jev reads it as true of answer; the rest stay
                    # quiet, and if it's true of nobody, nobody does. Everyone is told who it
                    # was for, so the rest don't take it as still open.
                    those = [c for c in members if reading.true_of.get(c, 0.0) >= TRUE_OF_FLOOR]
                    if those:
                        verb = "answers" if len(those) == 1 else "answer"
                        who = names([transcript.label(c) for c in those])
                        aside = f"{who} {verb} that; it isn't true of the others, who stay quiet."
                    else:
                        aside = "That isn't true of anyone at the table, so nobody answers."
                    takes = [Take(m, "chorus", NOTE_CHORUS_IF, USER) for m in those]
                    why = "chorus" if those else "nobody"
                    return Plan(takes, True, why, addressed=those, aside=aside)
                note = NOTE_CHORUS.format(who=everyone_or(members, transcript))
                takes = [Take(m, "chorus", note, USER) for m in members]
                return Plan(takes, True, "chorus", addressed=members)
            takes = group_takes(members, transcript, "group", NOTE_GROUP_FIRST, NOTE_GROUP_NEXT)
            return Plan(takes, why="group", addressed=members)
        if reading.choice in ids and reading.choice not in heard:
            if mark is not None and not mentions(mark.text, transcript.label(reading.choice)):
                # Unnamed, and too far away to hear it: Jev went by who the user spoke to last,
                # but they've walked away from them. It's for whoever is with them now.
                who = max(heard, key=lambda c: (reading.addressed(c), reading.included.get(c, 0.0)))
                note = None
                if last is not None and last.speaker != who:
                    note = NOTE_SWITCH.format(other=transcript.label(last.speaker))
                return Plan([Take(who, "addressed", note, USER)], why="addressed", addressed=[who])
            return self._not_heard(reading, heard, [reading.choice])
        if reading.choice in ids:
            # Talking to someone who didn't just speak: tell them so, or a correction such as
            # "no, I was asking the other one" reads to them as not meant for them.
            note = None
            if last is not None and last.speaker != reading.choice:
                note = NOTE_SWITCH.format(other=transcript.label(last.speaker))
            take = Take(reading.choice, "addressed", note, USER)
            return Plan([take], why="addressed", addressed=[reading.choice])
        who = last.speaker if last else heard[0]
        return Plan([Take(who, "fallback", None, USER)], why="fallback")

    def _not_heard(self, reading: Reading, heard: Sequence[str], asked: Sequence[str]) -> Plan:
        """The user spoke to someone too far away to hear: whoever who did hear is likeliest to
        be meant says so."""
        who = max(heard, key=lambda c: reading.addressed(c))
        other = names([self.transcript.label(c) for c in asked])
        take = Take(who, "not_heard", NOTE_NOT_HEARD.format(other=other), USER)
        return Plan([take], why="not_heard", addressed=[who])

    def after(self, reading: Reading, heard_by: frozenset[str] | None = None) -> Next:
        """A character's line (`reading.speaker`, already in the transcript) has been read: does
        someone answer it, and does anyone react to it out loud? Only those who heard it
        (`heard_by`, None: everyone) can."""
        energy = self.energy()
        take, why, drive = self._answer(reading, energy, heard_by)
        reaction = self._reaction(reading, take.speaker if take else None, heard_by)
        return Next(take, reaction, why, energy, drive)

    def _answer(
        self, reading: Reading, energy: float, heard_by: frozenset[str] | None = None
    ) -> tuple[Take | None, str, float | None]:
        transcript = self.transcript
        who, momentum = reading.choice, reading.momentum
        if reading.error or who is None or momentum is None:
            return None, "error", None
        if self._wrapping:
            return None, "wrapped", None
        if who == USER or who not in transcript.cast:
            return None, "user", None
        if heard_by is not None and who not in heard_by:
            return None, "unheard", None
        if reading.p(who) < REPLY_FLOOR and 1.0 - reading.p(USER) < TABLE_FLOOR:
            return None, "unsure", None
        if momentum < LANDED_MOMENTUM:
            return None, "landed", None
        drive = momentum * energy
        other = transcript.label(reading.speaker)
        if drive >= CONTINUE_FLOOR and self.talked() < MAX_TALK_S:
            take = Take(who, "reply", NOTE_REPLY.format(other=other), reading.speaker)
            return take, "reply", drive
        self._wrapping = True
        return Take(who, "wrap", NOTE_WRAP.format(other=other), reading.speaker), "wrap", drive

    def _reaction(
        self, reading: Reading, answering: str | None, heard_by: frozenset[str] | None = None
    ) -> Take | None:
        """Someone reacting out loud as the line ends, now and then, never twice running, and
        never whoever is about to answer it anyway, nor anyone who didn't hear it."""
        self._since_reaction += 1
        who = reading.reactor(REACT_FLOOR)
        if who in (None, reading.speaker, answering) or self._since_reaction <= REACT_COOLDOWN:
            return None
        if heard_by is not None and who not in heard_by:
            return None
        assert who is not None
        self._since_reaction = 0
        other = self.transcript.label(reading.speaker)
        return Take(who, "react", NOTE_REACT.format(other=other), reading.speaker)

    # --- Silence ----------------------------------------------------------------------------------

    def lull_wait(self) -> float | None:
        """How long the table waits on the user before someone picks something up, or None when
        it should stay quiet until they speak (they asked for quiet, or the waits have run out)."""
        if self._hushed or self._lulls >= len(LULL_WAITS_S):
            return None
        return LULL_WAITS_S[self._lulls]

    def lull(self, reading: Reading, near: Sequence[str] | None = None) -> Plan:
        """The user has said nothing for a while: someone near them (`near`, None: anyone)
        breaks the silence. Each of these exchanges counts LULL_HEAD_START_S more talk than the
        last, so it settles sooner."""
        ids = [c for c in self.transcript.cast if near is None or c in near]
        if not ids:
            return Plan(why="nobody")
        who = reading.choice
        if who not in ids:
            last = self.transcript.last_character()
            who = next((c for c in ids if c != last), ids[0])
        self._lulls += 1
        self._wrapping = False
        self._head_start += LULL_HEAD_START_S
        return Plan([Take(who, "lull", NOTE_LULL)], why="lull")

    # --- The host's other controls (the client's buttons) ----------------------------------------

    def fresh(self) -> None:
        """A new exchange, at full energy: e.g. a character drops in on another for a chat."""
        self._fresh()

    def hush(self) -> None:
        """Quiet: the table says nothing more until the user speaks."""
        self._hushed = True

    def carry_on(self, choice: str | None = None) -> Plan:
        """The table carries on, at full energy (see `_carry_on`)."""
        self._fresh()
        return self._carry_on(choice)

    def _carry_on(self, choice: str | None) -> Plan:
        """Whoever was cut off recently picks up where they were (the host cut in, maybe asked
        someone else something, then said "carry on"); otherwise `choice` if it's a character,
        or whoever spoke last."""
        ids = list(self.transcript.cast)
        recent = [ln for ln in self.transcript.lines[-CARRY_ON_LOOKBACK:] if ln.by_character]
        cut = next((ln for ln in reversed(recent) if ln.interrupted), None)
        last = recent[-1] if recent else None
        if cut is not None and choice not in ids:
            who, note = cut.speaker, NOTE_CARRY_ON_CUT
        else:
            who = choice if choice in ids else (last.speaker if last else ids[0])
            resumed = last is not None and last.interrupted and who == last.speaker
            note = NOTE_CARRY_ON_CUT if resumed else NOTE_CARRY_ON_TABLE
        assert who is not None
        return Plan([Take(who, "carry_on", note)], why="carry_on")
