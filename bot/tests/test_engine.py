"""The engine offline: routing the user's turns (one, a group in turn, a chorus, carry on, quiet),
what follows each line (an answer, a wrap-up, a reaction, or the user's turn), and silences."""

from __future__ import annotations

from pathlib import Path

import pytest

from config import (
    CONTINUE_FLOOR,
    ENERGY_HALF_LIFE_S,
    LULL_HEAD_START_S,
    LULL_WAITS_S,
    REACT_COOLDOWN,
    load_cast,
)
from engine import Engine
from room import (
    CARRY_ON,
    GROUP,
    HUSH,
    NOBODY,
    NOTE_CARRY_ON_CUT,
    NOTE_SWITCH,
    USER,
    Reading,
    Transcript,
)

# The kitchen table's six, whose routing these tests were written for.
CAST = load_cast(Path(__file__).parent / "characters.json")
IDS = [c.id for c in CAST]
MAYA, THEO, JUNO, OTTO = IDS[:4]
LINE = "That is a fine idea, but I think the soup needs more salt than that."  # 15 words


@pytest.fixture
def engine() -> Engine:
    t = Transcript(CAST)
    t.add(MAYA, "Hi, I'm Maya, and I love green.")
    t.add(OTTO, "Otto here. Blue, like the sea.")
    t.add(USER, "What should I cook tonight?")
    t.add(THEO, "Something simple.")
    return Engine(t)


def said(engine: Engine, text: str) -> None:
    engine.transcript.add(USER, text)


def route(
    choice: str | None,
    probabilities: dict[str, float] | None = None,
    included: dict[str, float] | None = None,
    *,
    chorus: float = 0.1,
    intent: dict[str, float] | None = None,
    error: str | None = None,
) -> Reading:
    return Reading(
        "route",
        USER,
        "…",
        choice,
        probabilities or ({choice: 0.9} if choice else {}),
        included or {},
        chorus=chorus,
        intent=intent or {"answer": 0.9, CARRY_ON: 0.05, HUSH: 0.05},
        error=error,
    )


def line(
    speaker: str,
    choice: str,
    p: float = 0.8,
    momentum: float = 2 / 3,
    react: dict[str, float] | None = None,
) -> Reading:
    odds = {choice: p, USER: 1 - p} if choice != USER else {USER: p}
    return Reading(
        "reply", speaker, "…", choice, odds, momentum=momentum, react=react or {NOBODY: 0.9}
    )


# --- The user's turn ---------------------------------------------------------------------------


def test_a_route_to_someone_new_tells_them(engine: Engine) -> None:
    said(engine, "Maya, any ideas?")
    [take] = engine.route(route(MAYA)).takes
    assert (take.speaker, take.note) == (MAYA, NOTE_SWITCH.format(other="Theo"))
    said(engine, "And you, Theo?")
    [take] = engine.route(route(THEO)).takes
    assert (take.speaker, take.note) == (THEO, None)


def test_a_group_answers_in_turn_most_surely_asked_first(engine: Engine) -> None:
    said(engine, "Otto and Juno, tell me a story each.")
    asked = {c: 0.1 for c in IDS} | {JUNO: 0.8, OTTO: 0.95}
    plan = engine.route(route(GROUP, {GROUP: 0.9}, asked, chorus=0.2))
    first, second = plan.takes
    assert not plan.together and plan.why == "group"
    assert [first.speaker, second.speaker] == [OTTO, JUNO]
    assert first.note and "said that to Otto and Juno. You answer first" in first.note
    assert second.note and "Otto has already replied. It's your turn now" in second.note
    assert engine.addressed == [OTTO, JUNO] and engine.favoured == GROUP


def test_a_quick_question_to_everyone_is_answered_all_at_once(engine: Engine) -> None:
    said(engine, "Who wants cake?")
    plan = engine.route(route(GROUP, {GROUP: 0.95}, {c: 0.9 for c in IDS}, chorus=0.85))
    assert plan.together and plan.why == "chorus"
    assert sorted(t.speaker for t in plan.takes) == sorted(IDS)
    assert all(t.reason == "chorus" and t.how == "together" for t in plan.takes)
    assert "everyone is answering at once" in (plan.takes[0].note or "")


def test_hands_up_if_only_those_it_is_true_of_answer(engine: Engine) -> None:
    said(engine, "Hands up if you've ever been on a boat.")
    reading = route(GROUP, {GROUP: 0.9}, {c: 0.9 for c in IDS}, chorus=0.9)
    reading.only_if = 0.85
    reading.true_of = {c: 0.1 for c in IDS} | {OTTO: 0.95, JUNO: 0.6}
    plan = engine.route(reading)
    assert plan.together and sorted(t.speaker for t in plan.takes) == sorted([OTTO, JUNO])
    # The rest are told it wasn't for them, so they don't answer it later.
    assert "Juno and Otto answer that" in engine.transcript.view(MAYA)[-1]["content"]
    # True of nobody: nobody answers (not everyone, as for a group nobody is sure of).
    said(engine, "Say yes if you've been to the moon.")
    reading = route(GROUP, {GROUP: 0.9}, {c: 0.9 for c in IDS}, chorus=0.9)
    reading.only_if, reading.true_of = 0.9, {c: 0.05 for c in IDS}
    assert engine.route(reading).takes == [] and engine.route(reading).why == "nobody"


def test_a_group_nobody_is_sure_of_is_everyone(engine: Engine) -> None:
    said(engine, "Well?")
    plan = engine.route(route(GROUP, {GROUP: 1.0}, {MAYA: 0.6}))
    assert [t.speaker for t in plan.takes] == IDS
    assert "everyone at the table" in (plan.takes[0].note or "")


def test_everyone_says_hi_at_once(engine: Engine) -> None:
    plan = engine.welcome()
    assert [t.speaker for t in plan.takes] == IDS and plan.together
    assert all(t.how == "together" and "says hi at once" in (t.note or "") for t in plan.takes)


def test_without_an_answer_the_last_speaker_carries_on(engine: Engine) -> None:
    said(engine, "Hmm.")
    [take] = engine.route(route(None, error="timeout")).takes
    assert (take.speaker, take.reason) == (THEO, "fallback")


def test_the_host_can_ask_for_quiet(engine: Engine) -> None:
    said(engine, "Shh, everyone, let me think.")
    plan = engine.route(route(GROUP, intent={"answer": 0.1, CARRY_ON: 0.05, HUSH: 0.85}))
    assert plan.takes == [] and plan.why == "hush"
    assert engine.lull_wait() is None  # nobody fills the silence until the user speaks again
    said(engine, "Right, Maya?")
    engine.route(route(MAYA))
    assert engine.lull_wait() == LULL_WAITS_S[0]


def test_the_host_can_tell_the_table_to_carry_on(engine: Engine) -> None:
    engine.transcript.add(JUNO, "And then the drummer", interrupted=True)
    said(engine, "Sorry, go on.")
    plan = engine.route(route(GROUP, intent={"answer": 0.1, CARRY_ON: 0.85, HUSH: 0.05}))
    [take] = plan.takes
    assert (take.speaker, take.reason, take.note) == (JUNO, "carry_on", NOTE_CARRY_ON_CUT)


def test_carry_on_goes_back_to_whoever_was_cut_off(engine: Engine) -> None:
    engine.transcript.add(THEO, "I'd sooner trust a seagull with", interrupted=True)
    said(engine, "Hang on, Edith, what do you think?")
    engine.route(route(IDS[5]))
    engine.transcript.add(IDS[5], "Back in my day, we cooked with memory.")
    said(engine, "Sorry, carry on.")
    plan = engine.route(route(IDS[5], intent={"answer": 0.0, CARRY_ON: 1.0, HUSH: 0.0}))
    [take] = plan.takes
    assert (take.speaker, take.note) == (THEO, NOTE_CARRY_ON_CUT)


# --- After a line ------------------------------------------------------------------------------


def test_a_character_answers_a_line_with_life_in_it(engine: Engine) -> None:
    engine.transcript.add(OTTO, "Theo couldn't boil an egg.")
    nxt = engine.after(line(OTTO, THEO))
    assert nxt.take is not None and (nxt.take.speaker, nxt.why) == (THEO, "reply")
    assert "Otto just spoke to you or about you" in (nxt.take.note or "")
    assert nxt.drive == pytest.approx((2 / 3) * nxt.energy)


def test_the_floor_goes_back_to_the_user_when_jev_says_so(engine: Engine) -> None:
    engine.transcript.add(OTTO, "What do you think, friend?")
    assert engine.after(line(OTTO, USER, 0.9)).why == "user"
    assert engine.after(line(OTTO, THEO, 0.45)).why == "unsure"
    assert engine.after(line(OTTO, THEO, 0.9, momentum=0.1)).why == "landed"
    assert engine.after(Reading("reply", OTTO, "…", None, error="timeout")).why == "error"


def test_a_lively_exchange_runs_on_and_then_winds_down_with_no_turn_count(engine: Engine) -> None:
    """Lines with the same life in them: answered while the table has energy (well past the
    old six), then one wrap-up, then the user's turn."""
    speakers, whys = [OTTO, THEO], []
    for i in range(40):
        speaker = speakers[i % 2]
        engine.transcript.add(speaker, LINE)
        nxt = engine.after(line(speaker, speakers[(i + 1) % 2]))
        whys.append(nxt.why)
        if nxt.take is None:
            break
    assert whys.count("reply") > 6
    assert whys[-2:] == ["wrap", "wrapped"]
    # It wrapped up once momentum × energy fell under the floor.
    assert (2 / 3) * engine.energy() < CONTINUE_FLOOR * 1.1


def test_a_mild_exchange_winds_down_sooner(engine: Engine) -> None:
    def length(momentum: float) -> int:
        t = Transcript(CAST)
        t.add(USER, "Go on then.")
        e = Engine(t)
        for i in range(40):
            speaker, other = (OTTO, THEO) if i % 2 else (THEO, OTTO)
            t.add(speaker, LINE)
            if e.after(line(speaker, other, 0.9, momentum)).why == "wrapped":
                return i
        return 40

    assert length(0.4) < length(0.7) < length(1.0)


def test_the_user_speaking_gives_the_table_fresh_energy(engine: Engine) -> None:
    for _ in range(10):
        engine.transcript.add(OTTO, LINE)
    tired = engine.energy()
    said(engine, "Juno, what do you think?")
    engine.route(route(JUNO))
    assert engine.energy() == 1.0 > tired
    # Ten 15-word lines and "Something simple." since the user last spoke.
    talked = (10 * 15 + 2) / 2.7
    assert tired == pytest.approx(0.5 ** (talked / ENERGY_HALF_LIFE_S))


def test_someone_reacts_now_and_then_but_never_the_one_answering(engine: Engine) -> None:
    laugh = {JUNO: 0.8, NOBODY: 0.2}
    engine.transcript.add(OTTO, "Theo's soup could strip paint.")
    nxt = engine.after(line(OTTO, THEO, react=laugh))
    assert nxt.reaction is not None and (nxt.reaction.speaker, nxt.reaction.how) == (
        JUNO,
        "reaction",
    )
    # Not again until the cooldown has passed...
    for _ in range(REACT_COOLDOWN):
        engine.transcript.add(THEO, "Paint is overrated.")
        assert engine.after(line(THEO, OTTO, react=laugh)).reaction is None
    engine.transcript.add(THEO, "Paint is overrated.")
    assert engine.after(line(THEO, OTTO, react=laugh)).reaction is not None
    # ...and never from whoever is answering anyway.
    for _ in range(REACT_COOLDOWN + 1):
        engine.transcript.add(OTTO, "Ha.")
        nxt = engine.after(line(OTTO, JUNO, react=laugh))
    assert nxt.take is not None and nxt.take.speaker == JUNO and nxt.reaction is None


# --- Silence -----------------------------------------------------------------------------------


def test_the_table_fills_a_few_silences_each_shorter_then_settles(engine: Engine) -> None:
    waits = []
    while (wait := engine.lull_wait()) is not None:
        waits.append(wait)
        before = engine.talked()
        plan = engine.lull(Reading("lull", USER, "", JUNO, {JUNO: 0.6}))
        assert [t.speaker for t in plan.takes] == [JUNO] and plan.takes[0].reason == "lull"
        assert engine.talked() == pytest.approx(before + LULL_HEAD_START_S)
    assert waits == list(LULL_WAITS_S)
    # Without Jev, someone other than whoever spoke last breaks it.
    [take] = engine.lull(Reading("lull", USER, "", None, error="timeout")).takes
    assert take.speaker != THEO


def test_carry_on_from_the_client_starts_afresh(engine: Engine) -> None:
    for _ in range(10):
        engine.transcript.add(OTTO, LINE)
    engine.hush()
    assert engine.lull_wait() is None
    [take] = engine.carry_on().takes
    assert take.speaker == OTTO and engine.energy() == 1.0
    assert engine.lull_wait() == LULL_WAITS_S[0]
