"""The room's logic offline: each character's view, Jev's state, recency, how long lines take
to say, and the Referee's requests and cache."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from pipecat.classifiers.base_classifier import (
    BaseClassifier,
    ChoiceQuestion,
    ChoiceResult,
    ClassifierResult,
    ScoreLevel,
    ScoreQuestion,
    ScoreResult,
    YesNoResult,
)

from config import load_cast
from room import (
    CARRY_ON,
    GROUP,
    HUSH,
    INTENT,
    NOBODY,
    REACTION,
    TOGETHER,
    USER,
    Reading,
    Referee,
    Transcript,
    could_be_silent,
    is_silent,
    seconds,
    weigh,
)

# The kitchen table's six, whose routing these tests were written for.
CAST = load_cast(Path(__file__).parent / "characters.json")
IDS = [c.id for c in CAST]
MAYA, THEO, JUNO, OTTO = IDS[:4]


@pytest.fixture
def transcript() -> Transcript:
    t = Transcript(CAST)
    t.add(MAYA, "Hi, I'm Maya, and I love green.")
    t.add(OTTO, "Otto here. Blue, like the sea.")
    t.add(USER, "What should I cook tonight?")
    t.add(THEO, "Something simple.")
    return t


def reading(
    choice: str, probabilities: dict[str, float], included: dict[str, float] | None = None
) -> Reading:
    return Reading("route", USER, "…", choice, probabilities, included or {})


def test_each_character_sees_their_own_lines_as_replies(transcript: Transcript) -> None:
    theo = transcript.view(THEO)
    assert [m["role"] for m in theo] == ["user", "assistant", "user"]
    assert theo[0]["content"] == (
        "[Maya] Hi, I'm Maya, and I love green.\n[Otto] Otto here. Blue, like the sea."
        "\n[User] What should I cook tonight?"
    )
    assert theo[-1]["content"] == "[Note: Carry on.]"
    # Her own opening line needs a user turn before it.
    maya = transcript.view(MAYA, "Say hi.")
    assert [m["role"] for m in maya] == ["user", "assistant", "user"]
    assert maya[-1]["content"].endswith("[Theo] Something simple.\n[Note: Say hi.]")


def test_a_line_cut_off_says_so(transcript: Transcript) -> None:
    transcript.add(JUNO, "Well, I think", interrupted=True)
    assert transcript.view(JUNO)[-2]["content"] == "Well, I think…"
    assert transcript.view(THEO)[-1]["content"].endswith("[Juno] Well, I think (cut off)")


def test_a_line_never_heard_is_taken_back(transcript: Transcript) -> None:
    line = transcript.add(JUNO, "Never said.")
    transcript.remove(line)
    assert "Never said." not in transcript.view(THEO)[-1]["content"]


def test_jev_sees_the_conversation_and_who_was_spoken_to(transcript: Transcript) -> None:
    state = transcript.for_jev(transcript.lines, transcript.lines[-1], [OTTO])
    assert state["user_last_spoke_to"] == "Otto"
    assert "Otto here. Blue, like the sea." in [line["said"] for line in state["conversation"]]
    assert list(state)[-1] == "latest"
    pair = transcript.for_jev([], transcript.lines[0], [MAYA, THEO])
    assert pair["user_last_spoke_to"] == "Maya and Theo"
    assert transcript.for_jev([], transcript.lines[0], IDS)["user_last_spoke_to"] == "everyone"
    assert "user_last_spoke_to" not in transcript.for_jev([], transcript.lines[0])
    quiet = transcript.for_jev(transcript.lines, transcript.lines[-1], quiet_for=9.6)
    assert quiet["quiet_for_seconds"] == 10 and "latest" not in quiet


def test_a_chorus_takes_as_long_as_its_longest_line_and_a_reaction_takes_none(
    transcript: Transcript,
) -> None:
    assert [ln.speaker for ln in transcript.since_user()] == [THEO]
    long = "Yes please, a big slice with cream on top"
    transcript.add(JUNO, long, how=TOGETHER, chorus=1)
    transcript.add(OTTO, "Aye.", how=TOGETHER, chorus=1)
    transcript.add(MAYA, "Ha!", how=REACTION)
    talked = Transcript.talked(transcript.since_user())
    assert talked == pytest.approx(seconds("Something simple.") + seconds(long))


def test_a_light_recency_prior_only_settles_near_ties() -> None:
    torn = weigh(reading(MAYA, {MAYA: 0.48, THEO: 0.44, GROUP: 0.08}), THEO, 1.2)
    assert torn.choice == THEO
    assert torn.raw == {MAYA: 0.48, THEO: 0.44, GROUP: 0.08}
    assert sum(torn.probabilities.values()) == pytest.approx(1.0)
    leaning = weigh(reading(MAYA, {MAYA: 0.6, THEO: 0.35, GROUP: 0.05}), THEO, 1.2)
    assert leaning.choice == MAYA
    unchanged = reading(MAYA, {MAYA: 0.6, THEO: 0.4})
    assert weigh(unchanged, None, 1.2) is unchanged
    assert weigh(unchanged, THEO, 1.0) is unchanged


def test_what_the_host_wants_needs_jev_to_be_sure() -> None:
    sure = Reading("route", USER, "…", None, intent={"answer": 0.2, CARRY_ON: 0.1, HUSH: 0.7})
    assert sure.wants(0.5) == HUSH
    unsure = Reading("route", USER, "…", None, intent={"answer": 0.55, CARRY_ON: 0.45})
    assert unsure.wants(0.5) == "answer"


def test_a_reaction_needs_jev_to_be_sure_someone_reacts() -> None:
    laugh = Reading("reply", THEO, "…", USER, react={JUNO: 0.7, NOBODY: 0.3})
    assert laugh.reactor(0.6) == JUNO
    assert Reading("reply", THEO, "…", USER, react={JUNO: 0.55, NOBODY: 0.45}).reactor(0.6) is None
    assert Reading("reply", THEO, "…", USER, react={NOBODY: 0.9}).reactor(0.6) is None


class CountingClassifier(BaseClassifier):
    """Answers every question the same way, slowly, and counts the requests."""

    def __init__(self) -> None:
        super().__init__()
        self.calls = 0

    async def _ask(self, state, questions):
        self.calls += 1
        await asyncio.sleep(0.05)
        results: dict[str, ClassifierResult] = {}
        for name, question in questions.items():
            if isinstance(question, ScoreQuestion):
                levels = [ScoreLevel(level=lv, probability=0.25) for lv in question.levels]
                results[name] = ScoreResult(score=2.0, levels=levels, confidence=0.5)
            elif isinstance(question, ChoiceQuestion):
                odds = {option: 0.0 for option in question.options}
                pick = {
                    "next": JUNO,
                    "react": NOBODY,
                    INTENT: "answer",
                    "lull": OTTO,
                }.get(name, GROUP)
                odds |= {pick: 0.7}
                results[name] = ChoiceResult(choice=pick, probabilities=odds, confidence=0.5)
            elif name == "chorus":
                results[name] = YesNoResult(probability=0.8)
            else:
                p = 0.9 if name.endswith((THEO, OTTO)) else 0.1
                results[name] = YesNoResult(probability=p)
        return results, None


async def test_one_request_answers_the_choice_every_inclusion_the_chorus_and_the_intent(
    transcript: Transcript,
) -> None:
    classifier = CountingClassifier()
    referee = Referee(classifier, CAST)
    history = list(transcript.lines)
    # The partial's read is still in flight when the turn ends: the final one shares it.
    preview = asyncio.create_task(
        referee.addressee(transcript, history, "and then", kind="preview")
    )
    await asyncio.sleep(0)
    final = await referee.addressee(transcript, history, "and  then")
    assert (await preview).choice == final.choice == GROUP
    assert classifier.calls == 1
    assert final.included == {c: 0.9 if c in (THEO, OTTO) else 0.1 for c in IDS}
    assert final.group(IDS) == [THEO, OTTO]
    assert final.addressed(THEO) == pytest.approx(0.7 * 0.9)  # as one of the group
    assert final.chorus == 0.8 and final.wants(0.5) == "answer"
    # Once it has landed, the same words cost nothing.
    again = await referee.addressee(transcript, history, "and then")
    assert again.cached and again.ms == 0 and classifier.calls == 1
    # Who the user spoke to last is part of the question, so it is asked afresh.
    await referee.addressee(transcript, history, "and then", last_addressed=[THEO])
    assert classifier.calls == 2


async def test_jev_is_told_who_heard_the_line_it_reads(transcript: Transcript) -> None:
    referee = Referee(CountingClassifier(), CAST)
    history = list(transcript.lines)
    route = await referee.addressee(
        transcript, history, "what's that?", last_addressed=[OTTO], heard_by=frozenset({THEO})
    )
    latest = route.state["latest"]
    assert latest["heard_by"] == [transcript.label(THEO)]
    assert transcript.label(OTTO) in latest["out_of_earshot"]  # spoken to last, walked away from
    reply = await referee.reply(transcript, history, THEO, "Hm?", heard_by=frozenset({OTTO}))
    assert reply.state["latest"]["heard_by"] == [transcript.label(OTTO)]


async def test_a_reply_read_asks_who_answers_the_momentum_and_who_reacts(
    transcript: Transcript,
) -> None:
    classifier = CountingClassifier()
    referee = Referee(classifier, CAST)
    history = list(transcript.lines)
    # Asked while the line is still playing, then again when it ends: one request.
    early = asyncio.create_task(referee.reply(transcript, history, THEO, "Juno, sing!"))
    await asyncio.sleep(0)
    late = await referee.reply(transcript, history, THEO, "Juno,  sing!")
    assert (await early).choice == late.choice == JUNO
    assert classifier.calls == 1
    assert late.momentum == pytest.approx(2 / 3)  # the third of four levels
    assert late.reactor(0.6) is None
    # The speaker is never an option: they don't answer, or react to, themselves.
    assert THEO not in late.probabilities and USER in late.probabilities
    assert THEO not in late.react and NOBODY in late.react


async def test_a_lull_read_asks_who_breaks_the_silence(transcript: Transcript) -> None:
    referee = Referee(CountingClassifier(), CAST)
    reading = await referee.lull(transcript, 12.0)
    assert reading.kind == "lull" and reading.choice == OTTO
    assert reading.state["quiet_for_seconds"] == 12


def test_a_character_can_choose_to_say_nothing() -> None:
    assert is_silent("[silent]") and is_silent(" [Silent] ") and is_silent("[silent] no")
    assert not is_silent("Yes please!")
    # While it's being written, a line is held back only while it could still be silence.
    assert could_be_silent("") and could_be_silent(" [sil") and could_be_silent("[silent]")
    assert not could_be_silent("I") and not could_be_silent("[s] yes")
