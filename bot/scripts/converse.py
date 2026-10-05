"""The room in text, on the live Jev and LLM: no audio, the same engine as the bot. Nobody
walks around: everyone hears everything, as at the kitchen table, which makes it the place to
try the characters' prompts and the engine's tunables.

    uv run python scripts/converse.py                       # hellos, then type as the host
    uv run python scripts/converse.py --script FILE         # one host line per line of FILE
    uv run python scripts/converse.py --watch "Who wants cake?"   # say it, then stay quiet
    uv run python scripts/converse.py --soak scripts/topics.txt   # many openers: how they end

Time is simulated: each line takes as long as its words take to say (`WORDS_PER_S`), a chorus as
long as its longest line, and a silence as long as the engine waits before someone fills it.

As the host (typed, or in a script):
    text        say it to the table
    ...         say nothing: someone may fill the silence (an empty line does too, typed)
    !N text     cut in N character lines into what follows, with `text`
    # text      a comment (scripts)
    q           quit (typed)
"""

from __future__ import annotations

import argparse
import asyncio
import io
import statistics
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from openai import AsyncOpenAI
from pipecat.classifiers.jev.classifier import JevClassifier

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import services  # noqa: E402
from cast import prompt  # noqa: E402
from config import (  # noqa: E402
    LLM_TEMPERATURE,
    LLM_TOKENS,
    RECENCY_WEIGHT,
    TURN_GAP_S,
    Settings,
    load_cast,
    load_environment,
)
from engine import Engine, Next, Plan, Take, strongest  # noqa: E402
from room import (  # noqa: E402
    USER,
    Line,
    Reading,
    Referee,
    Transcript,
    is_silent,
    normalize,
    seconds,
    weigh,
)

DIM, BOLD, RED, RESET = "\033[2m", "\033[1m", "\033[31m", "\033[0m"


@dataclass
class Outcome:
    """How one stretch of the table talking went."""

    why: str  # how it ended: the engine's reason, or "cut" (the host cut in)
    lines: int = 0  # character lines, reactions not counted
    reactions: int = 0
    talked: float = 0.0  # seconds


@dataclass
class Session:
    """One conversation's numbers, for a soak."""

    opener: str
    exchanges: list[Outcome] = field(default_factory=list)
    lulls: int = 0
    seconds: float = 0.0  # from the host's line until the table settles
    repeats: int = 0


def similar(a: str, b: str) -> bool:
    """Two lines saying much the same thing (word overlap)."""
    x, y = set(normalize(a).lower().split()), set(normalize(b).lower().split())
    return bool(x and y) and len(x & y) / len(x | y) >= 0.7


class Table:
    """The engine, played out in text."""

    def __init__(
        self, cast, settings: Settings, referee: Referee, client: AsyncOpenAI, out=None
    ) -> None:
        self.cast = {c.id: c for c in cast}
        self.prompts = {c.id: prompt(c, cast) for c in cast}
        self.settings = settings
        self.referee = referee
        self.client = client
        self.transcript = Transcript(cast)
        self.engine = Engine(self.transcript)
        self.clock = 0.0
        self.out = out or sys.stdout
        self.repeats = 0
        self._choruses = 0

    def print(self, text: str = "") -> None:
        print(text, file=self.out, flush=True)

    def stamp(self) -> str:
        return f"{DIM}{int(self.clock // 60):02d}:{self.clock % 60:04.1f}{RESET}"

    async def write(self, take: Take) -> tuple[str, float]:
        messages = [{"role": "system", "content": self.prompts[take.speaker]}]
        messages += self.transcript.view(take.speaker, take.note)
        started = time.perf_counter()
        if self.settings.phonellm:
            reply = await self.client.chat.completions.create(
                model=self.settings.llm_model,
                messages=messages,  # type: ignore[arg-type]
                temperature=LLM_TEMPERATURE,
                max_tokens=LLM_TOKENS,
                extra_body=services.extra_body(self.settings),
            )
        else:
            reply = await self.client.chat.completions.create(
                model=self.settings.llm_model,
                messages=messages,  # type: ignore[arg-type]
                max_tokens=LLM_TOKENS,
            )
        text = normalize(reply.choices[0].message.content or "")
        return text, (time.perf_counter() - started) * 1000

    def say(self, take: Take, text: str, ms: float, chorus: int | None = None) -> Line:
        earlier = [ln.text for ln in self.transcript.lines if ln.by_character]
        repeat = any(similar(text, e) for e in earlier)
        self.repeats += repeat
        line = self.transcript.add(take.speaker, text, how=take.how, chorus=chorus)
        tag = {"together": " ⟂", "reaction": " ↯"}.get(take.how, "")
        flag = f" {RED}(repeat){RESET}" if repeat else ""
        name = self.cast[take.speaker].name
        self.print(
            f"{self.stamp()} {BOLD}{name}{RESET}{tag} {DIM}({take.reason}, {ms:.0f} ms){RESET}:"
            f" {text}{flag}"
        )
        return line

    def show(self, reading: Reading, nxt: Next | None = None) -> None:
        top = sorted(reading.probabilities.items(), key=lambda kv: -kv[1])[:3]
        odds = "  ".join(f"{k} {v:.2f}" for k, v in top)
        bits = [odds]
        if reading.choice == "group":
            asked = [k for k, v in reading.included.items() if v >= 0.5]
            bits.append(
                f"asked: {' '.join(asked)}  chorus {reading.chorus or 0:.2f}"
                f"  only-if {reading.only_if or 0:.2f}"
            )
        if reading.intent:
            bits.append(" ".join(f"{k} {v:.2f}" for k, v in reading.intent.items()))
        if nxt is not None:
            react = reading.reactor(0.0)
            bits.append(
                f"momentum {reading.momentum or 0:.2f} × energy {nxt.energy:.2f}"
                f"{f'  react {react} {reading.react.get(react, 0):.2f}' if react else ''}"
            )
            bits.append(f"→ {nxt.take.speaker if nxt.take else 'user'} ({nxt.why})")
        cached = " cached" if reading.cached else ""
        error = f" {RED}ERROR {reading.error}{RESET}" if reading.error else ""
        self.print(
            f"{DIM}      jev {reading.kind:<6} {reading.ms:4.0f} ms{cached}  "
            f"{' | '.join(bits)}{RESET}{error}"
        )

    # --- Playing plans --------------------------------------------------------------------------

    async def play(self, plan: Plan, cut_after: int | None = None) -> Outcome:
        """Play a plan, and whatever the table says after it, until the floor is the host's
        (or the host cuts in `cut_after` character lines in)."""
        outcome = Outcome(plan.why)
        start = self.clock
        if not plan.takes:
            return outcome
        last: Line | None = None
        if plan.together:
            self._choruses += 1
            written = await asyncio.gather(*(self.write(t) for t in plan.takes))
            for take, (text, ms) in zip(plan.takes, written, strict=True):
                if is_silent(text):
                    self.print(f"{DIM}      ({self.cast[take.speaker].name} stays quiet){RESET}")
                elif text:
                    last = self.say(take, text, ms, chorus=self._choruses)
                    outcome.lines += 1
            spoken = [t for t, _ in written if not is_silent(t)]
            self.clock += max((seconds(t) for t in spoken), default=0.0) + TURN_GAP_S
        else:
            for take in plan.takes:
                text, ms = await self.write(take)
                if text:
                    last = self.say(take, text, ms)
                    outcome.lines += 1
                    if self.cut(last, outcome, cut_after):
                        return self.ended(outcome, "cut", start)
        chorus = plan.together
        while last is not None:
            if chorus:  # every line of it is read; the likeliest to get an answer is followed
                lines = [ln for ln in self.transcript.lines if ln.chorus == self._choruses]
                reading = strongest(await asyncio.gather(*(self.read(ln) for ln in lines)))
                chorus = False
            else:
                reading = await self.read(last)
            nxt = self.engine.after(reading)
            self.show(reading, nxt)
            if nxt.reaction is not None:
                text, ms = await self.write(nxt.reaction)
                if text:
                    self.say(nxt.reaction, text, ms)
                    outcome.reactions += 1
            if nxt.take is None:
                return self.ended(outcome, nxt.why, start)
            text, ms = await self.write(nxt.take)
            if not text:
                return self.ended(outcome, "empty", start)
            last = self.say(nxt.take, text, ms)
            outcome.lines += 1
            if self.cut(last, outcome, cut_after):
                return self.ended(outcome, "cut", start)
        return self.ended(outcome, outcome.why, start)

    async def read(self, line: Line) -> Reading:
        history = self.transcript.lines[: self.transcript.lines.index(line)]
        return await self.referee.reply(self.transcript, history, line.speaker, line.text)

    def cut(self, line: Line, outcome: Outcome, cut_after: int | None) -> bool:
        """Advance the clock past a line; or, if the host cuts in here, halfway through it."""
        if cut_after is not None and outcome.lines >= cut_after:
            words = line.text.split()
            line.text, line.interrupted = " ".join(words[: max(1, len(words) // 2)]), True
            self.clock += seconds(line.text)
            self.print(f"{DIM}      ⌁ cut off at “{line.text}”{RESET}")
            return True
        self.clock += seconds(line.text) + TURN_GAP_S
        return False

    def ended(self, outcome: Outcome, why: str, start: float) -> Outcome:
        outcome.why, outcome.talked = why, self.clock - start
        if why != "cut":
            self.print(
                f"{DIM}      ── {why}: {outcome.lines} lines"
                f"{f', {outcome.reactions} reactions' if outcome.reactions else ''},"
                f" {outcome.talked:.0f} s ──{RESET}"
            )
        return outcome

    # --- The host -------------------------------------------------------------------------------

    async def host(self, said: str, cut_after: int | None = None) -> Outcome:
        """The host says something to the table."""
        history = list(self.transcript.lines)
        self.transcript.add(USER, said)
        self.print(f"{self.stamp()} {BOLD}You{RESET}: {said}")
        self.clock += seconds(said) + TURN_GAP_S
        engine = self.engine
        reading = await self.referee.addressee(
            self.transcript, history, said, last_addressed=engine.addressed
        )
        reading = weigh(reading, engine.favoured, RECENCY_WEIGHT)
        plan = engine.route(reading)
        self.show(reading)
        self.print(f"{DIM}      → {plan.why}{RESET}")
        return await self.play(plan, cut_after)

    async def quiet(self, cut_after: int | None = None) -> Outcome | None:
        """The host says nothing: someone fills the silence, if the engine allows."""
        wait = self.engine.lull_wait()
        if wait is None:
            self.print(f"{DIM}      (the table stays quiet){RESET}")
            return None
        self.clock += wait
        reading = await self.referee.lull(self.transcript, wait)
        plan = self.engine.lull(reading)
        self.show(reading)
        self.print(f"{DIM}      → {wait:.0f} s of quiet; {plan.takes[0].speaker} fills it{RESET}")
        return await self.play(plan, cut_after)

    async def step(self, entry: str, cut_after: int | None = None) -> Outcome | None:
        if entry in ("", "..."):
            return await self.quiet(cut_after)
        return await self.host(entry, cut_after)

    async def settle(self, session: Session | None = None) -> None:
        """Stay quiet until the table does."""
        while (outcome := await self.quiet()) is not None:
            if session is not None:
                session.lulls += 1
                session.exchanges.append(outcome)


def parse(lines: list[str]) -> list[tuple[str, int | None]]:
    """Script entries, each with the cut-in that interrupts what follows it (if any)."""
    entries: list[str] = [ln.strip() for ln in lines if not ln.strip().startswith("#")]
    entries = [e for e in entries if e]
    steps: list[tuple[str, int | None]] = []
    for i, entry in enumerate(entries):
        if entry.startswith("!"):
            entry = entry.split(" ", 1)[1] if " " in entry else "..."
        nxt = entries[i + 1] if i + 1 < len(entries) else ""
        cut_after = None
        if nxt.startswith("!"):
            head = nxt.split(" ", 1)[0][1:]
            cut_after = int(head) if head.isdigit() else 1
        steps.append((entry, cut_after))
    return steps


async def soak(make_table, openers: list[str], parallel: int, verbose: bool) -> None:
    gate = asyncio.Semaphore(parallel)

    async def one(opener: str) -> Session:
        async with gate:
            buffer = io.StringIO()
            table: Table = make_table(None if verbose else buffer)
            session = Session(opener)
            session.exchanges.append(await table.host(opener))
            await table.settle(session)
            session.seconds = table.clock
            session.repeats = table.repeats
            print(f"{DIM}  done: {opener}{RESET}", flush=True)
            return session

    sessions = await asyncio.gather(*(one(o) for o in openers))
    print()
    print(f"{BOLD}{'opener':<46} lines  talk   first exchange        lulls  repeats{RESET}")
    for s in sessions:
        lines = sum(o.lines for o in s.exchanges)
        first = s.exchanges[0]
        print(
            f"{s.opener[:45]:<46} {lines:>5} {s.seconds:>5.0f}s  "
            f"{first.lines:>2} lines, {first.why:<10} {s.lulls:>5} {s.repeats:>8}"
        )
    firsts = [s.exchanges[0] for s in sessions]
    endings: dict[str, int] = {}
    for o in (o for s in sessions for o in s.exchanges):
        endings[o.why] = endings.get(o.why, 0) + 1
    print()
    print(
        f"first exchange: median {statistics.median(o.lines for o in firsts):.0f} lines, "
        f"{statistics.median(o.talked for o in firsts):.0f} s; "
        f"longest {max(o.lines for o in firsts)} lines, {max(o.talked for o in firsts):.0f} s"
    )
    print(
        f"until settled: median {statistics.median(s.seconds for s in sessions):.0f} s, "
        f"longest {max(s.seconds for s in sessions):.0f} s"
    )
    print("endings: " + ", ".join(f"{k} {v}" for k, v in sorted(endings.items())))
    total = sum(o.lines for s in sessions for o in s.exchanges)
    print(f"repeats: {sum(s.repeats for s in sessions)} of {total} lines")


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path)
    parser.add_argument("--watch", metavar="LINE")
    parser.add_argument("--soak", type=Path, metavar="FILE")
    parser.add_argument("--parallel", type=int, default=3)
    parser.add_argument(
        "--no-welcome", action="store_true", help="skip the hellos (scripts, typing)"
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="soak: show every line")
    args = parser.parse_args()

    load_environment()
    settings = Settings.from_env()
    cast = load_cast()
    jev = services.jev(settings)
    referee = Referee(JevClassifier(client=jev), cast)
    client = AsyncOpenAI(api_key=settings.llm_api_key, base_url=settings.llm_base_url)

    def make_table(out=None) -> Table:
        return Table(cast, settings, referee, client, out)

    print(f"{DIM}PhoneLLM {settings.llm_model}, Jev {referee.model}{RESET}")
    await services.warm_llm(settings)
    try:
        if args.soak:
            openers = [
                ln.strip()
                for ln in args.soak.read_text().splitlines()
                if ln.strip() and not ln.startswith("#")
            ]
            await soak(make_table, openers, args.parallel, args.verbose)
            return
        table = make_table()
        if args.watch:
            await table.host(args.watch)
            await table.settle()
            return
        if not args.no_welcome:
            await table.play(table.engine.welcome())
        if args.script:
            for entry, cut_after in parse(args.script.read_text().splitlines()):
                await table.step(entry, cut_after)
            return
        while True:
            try:
                said = input(f"{BOLD}You{RESET}: ").strip()
            except EOFError:
                break
            if said == "q":
                break
            await table.step(said)
    finally:
        await referee.close()
        await jev.close()
        await client.close()


if __name__ == "__main__":
    asyncio.run(main())
