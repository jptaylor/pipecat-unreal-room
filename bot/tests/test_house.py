"""The house, offline: only those close enough hear the user, or a character, and only they
answer; the user can ask whoever they're talking to to move; and the game is told how they feel
and what they do."""

from __future__ import annotations

from typing import Any

import pytest

from config import load_cast
from director import Director
from engine import Engine, Plan, Take
from room import (
    ANSWER,
    CARRY_ON,
    GROUP,
    HUSH,
    NOBODY,
    STAY,
    USER,
    Reading,
    Transcript,
    mentions,
)
from space import Space

CAST = load_cast()
MAYA, THEO, JUNO = (c.id for c in CAST)
AREAS = {"areas": [{"id": a, "name": f"the {a}"} for a in ("hall", "kitchen", "conservatory")]}


def route(choice: str, **fields: Any) -> Reading:
    reading = Reading(
        "route",
        USER,
        "…",
        choice,
        {choice: 0.9},
        intent={ANSWER: 0.9, CARRY_ON: 0.05, HUSH: 0.05},
    )
    for name, value in fields.items():
        setattr(reading, name, value)
    return reading


def engine_with(heard: set[str], said: str = "Hello?") -> Engine:
    t = Transcript(CAST)
    t.add(USER, said, heard_by=frozenset(heard))
    return Engine(t)


# --- Who hears what ---------------------------------------------------------------------------


def test_before_the_game_says_anything_everyone_hears_everything() -> None:
    space = Space(CAST)
    assert space.heard_user() == {MAYA, THEO, JUNO}
    assert space.listeners(MAYA) == {THEO, JUNO, USER}
    assert space.near_user() == {MAYA, THEO, JUNO}
    assert space.situation(MAYA) == ""


def test_the_game_says_who_hears_whom() -> None:
    space = Space(CAST)
    space.world(AREAS)
    space.update(
        {
            "user": {"area": "kitchen"},
            "characters": {
                MAYA: {"area": "conservatory", "hears": [], "intent": "home"},
                THEO: {"area": "kitchen", "hears": [USER, JUNO], "intent": "home"},
                JUNO: {"area": "kitchen", "hears": [USER, THEO], "intent": "follow"},
            },
        }
    )
    space.earshot({"heard": [THEO, "nobody we know"]})
    assert space.heard_user() == {THEO}
    assert space.listeners(THEO) == {USER, JUNO}
    assert space.near_user() == {THEO, JUNO}
    assert space.situation(THEO) == (
        "You're in the kitchen, with Juno. Maya (in the conservatory) isn't close enough to hear "
        "you. The person is here with you, close enough to talk to. You haven't met the person "
        "before: if they say hello, say hello back, give your name and ask theirs, in a few words."
    )
    space.talked_with(THEO)
    assert space.situation(THEO) == (
        "You're in the kitchen, with Juno. Maya (in the conservatory) isn't close enough to hear "
        "you. The person is here with you, close enough to talk to."
    )
    assert "following the person" in space.situation(JUNO)
    assert "The person is in the kitchen, too far away to hear you." in space.situation(MAYA)


def test_only_those_the_user_has_met_speak_up_unasked() -> None:
    space = Space(CAST)
    space.world(AREAS)
    space.update(
        {
            "user": {"area": "kitchen"},
            "characters": {
                MAYA: {"area": "kitchen", "hears": [USER, THEO]},
                THEO: {"area": "kitchen", "hears": [USER, MAYA]},
                JUNO: {"area": "hall", "hears": []},
            },
        }
    )
    assert space.friends_near_user() == set()
    assert space.talked_with(MAYA) and not space.talked_with(MAYA)  # new only the first time
    assert space.friends_near_user() == {MAYA}
    # Jev knows whom the user knows, and whom they've yet to meet.
    state = space.for_jev()
    assert state["user_has_talked_with"] == ["Maya"]
    assert state["user_has_not_met"] == ["Theo", "Juno"]


async def test_coming_up_to_someone_isnt_a_cue_to_speak(director: Director, monkeypatch) -> None:
    played: list[Plan] = []

    async def play(plan: Plan, *args: Any, **kwargs: Any) -> None:
        played.append(plan)

    monkeypatch.setattr(director, "play", play)
    await director.met(THEO)
    assert played == []


def test_a_character_is_shown_only_what_they_heard() -> None:
    t = Transcript(CAST)
    t.add(USER, "Maya, hi!", heard_by=frozenset({MAYA}))
    t.add(MAYA, "Hello there!", heard_by=frozenset({USER}))
    t.add(USER, "Theo, what's cooking?", heard_by=frozenset({THEO}))
    assert [m["content"] for m in t.view(THEO)] == ["[User] Theo, what's cooking?"]
    assert t.view(MAYA)[0]["content"] == "[User] Maya, hi!"
    assert "Theo, what's cooking" not in str(t.view(MAYA))


# --- The user's turn ---------------------------------------------------------------------------


def test_nobody_answers_what_nobody_heard() -> None:
    assert engine_with(set()).route(route(MAYA)).why == "unheard"


def test_someone_too_far_away_is_told_about_by_whoever_heard() -> None:
    plan = engine_with({THEO}, "Maya, are you there?").route(route(MAYA))
    assert plan.why == "not_heard" and [t.speaker for t in plan.takes] == [THEO]
    assert plan.takes[0].note and "Maya, who is too far away to hear it" in plan.takes[0].note
    # Speech-to-text's take on the name still names her.
    assert engine_with({THEO}, "Mya, are you there?").route(route(MAYA)).why == "not_heard"


def test_whoever_the_user_walked_away_from_isnt_who_theyre_talking_to() -> None:
    """Jev picks Maya, whom the user spoke to last, but she's out of earshot and wasn't named:
    the user is talking to whoever is with them now."""
    plan = engine_with({THEO}, "What are you cooking?").route(route(MAYA))
    assert plan.why == "addressed" and [t.speaker for t in plan.takes] == [THEO]


def test_a_name_is_a_name_and_the_is_not_theo() -> None:
    assert mentions("Juno, play us something", "Juno")
    assert mentions("junot can you hear me", "Juno")
    assert not mentions("what's the plan?", "Theo")
    assert not mentions("may I have some cake?", "Maya")


def test_a_group_is_those_of_it_who_heard() -> None:
    included = {MAYA: 0.9, THEO: 0.9, JUNO: 0.9}
    plan = engine_with({THEO, JUNO}).route(route(GROUP, included=included, chorus=0.1))
    assert plan.why == "group" and [t.speaker for t in plan.takes] == [THEO, JUNO]
    alone = engine_with({JUNO}).route(route(GROUP, included=included))
    assert alone.why == "addressed" and [t.speaker for t in alone.takes] == [JUNO]


def test_whoever_answers_the_user_is_talking_to_them() -> None:
    plan = engine_with({THEO}).route(route(THEO))
    assert plan.takes[0].to == USER


# --- After a line ------------------------------------------------------------------------------


def test_only_those_who_heard_a_line_answer_or_react_to_it() -> None:
    engine = engine_with({MAYA, THEO, JUNO})
    engine.route(route(THEO))
    engine.transcript.add(THEO, "Maya, these tomatoes are tragic.", heard_by=frozenset({USER}))
    reading = Reading(
        "reply", THEO, "…", MAYA, {MAYA: 0.9, USER: 0.1}, momentum=1.0, react={JUNO: 0.9}
    )
    nxt = engine.after(reading, frozenset({USER}))
    assert nxt.take is None and nxt.why == "unheard" and nxt.reaction is None
    heard = engine.after(reading, frozenset({MAYA, USER}))
    assert heard.take is not None and heard.take.speaker == MAYA and heard.take.to == THEO


def test_a_lull_is_broken_only_by_someone_near_the_user() -> None:
    engine = engine_with({THEO})
    lull = Reading("lull", USER, "", MAYA, {MAYA: 0.9})
    assert engine.lull(lull, [THEO]).takes[0].speaker == THEO
    assert engine.lull(lull, []).why == "nobody"


# --- Moving, and how they feel -------------------------------------------------------------------


def test_a_move_is_only_what_jev_is_sure_of() -> None:
    assert route(THEO, move={"follow": 0.8}).wants_move(0.6) == ("follow", None)
    assert route(THEO, move={"go:kitchen": 0.7}).wants_move(0.6) == ("go", "kitchen")
    assert route(THEO, move={"follow": 0.5, STAY: 0.5}).wants_move(0.6) is None
    assert route(THEO, move={STAY: 0.9}).wants_move(0.6) is None


@pytest.fixture
def director() -> Director:
    director = Director(CAST, referee=None)  # type: ignore[arg-type]
    director.space.world(AREAS)
    return director


def test_whoever_is_asked_to_move_is_told_so_and_the_game_moves_them(
    director: Director,
) -> None:
    plan = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    move = director._with_move(plan, route(THEO, move={"follow": 0.9}))
    assert move == {"type": "move", "who": [THEO], "action": "follow", "area": None}
    assert plan.takes[0].note == "As you answer, you start following the person, wherever they go."


def test_a_character_can_be_sent_somewhere_or_home(director: Director) -> None:
    plan = Plan([Take(JUNO, "addressed", None, USER)], why="addressed", addressed=[JUNO])
    move = director._with_move(plan, route(JUNO, move={"go:kitchen": 0.9}))
    assert move is not None and move["action"] == "go" and move["area"] == "kitchen"
    assert plan.takes[0].note == "As you answer, you head to the kitchen."
    home = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    assert director._with_move(home, route(MAYA, move={"home": 0.9})) == {
        "type": "move",
        "who": [MAYA],
        "action": "home",
        "area": None,
    }
    assert home.takes[0].note == "As you answer, you head back to the conservatory."


def test_everyone_asked_to_follow_follows_even_if_only_one_answers(director: Director) -> None:
    plan = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    reading = route(THEO, move={"follow": 0.9}, included={THEO: 0.9, MAYA: 0.85, JUNO: 0.1})
    move = director._with_move(plan, reading)
    assert move is not None and move["who"] == [MAYA, THEO]


def test_whoever_the_user_walked_away_from_isnt_favoured(director: Director) -> None:
    director.engine.favoured = MAYA
    assert director._favoured({MAYA, THEO}) == MAYA
    assert director._favoured({THEO}) is None


def test_an_errand_takes_them_there_and_isnt_a_move_too(director: Director) -> None:
    plan = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    reading = route(THEO, move={"go:conservatory": 0.8}, act={"food": 0.9})
    assert director._with_move(plan, reading) is None
    assert director._with_move(plan, route(THEO, move={"go:conservatory": 0.8})) is not None


def test_nobody_moves_on_a_turn_they_didnt_hear(director: Director) -> None:
    plan = Plan([Take(THEO, "not_heard", "…", USER)], why="not_heard", addressed=[THEO])
    assert director._with_move(plan, route(MAYA, move={"come": 0.9})) is None


async def test_the_listeners_show_how_they_take_what_the_user_says(director: Director) -> None:
    sent: list[dict[str, Any]] = []

    async def emit(data: dict[str, Any]) -> None:
        sent.append(data)

    director.emit = emit  # type: ignore[method-assign]
    director.space.earshot({"heard": [THEO, JUNO]})
    await director._listening(route(THEO, feel={"laughing": 0.8, "neutral": 0.2}))
    assert sent == [
        {
            "type": "emote",
            "who": [JUNO, THEO],
            "mood": "laughing",
            "strength": 0.48,
            "role": "listen",
            "target": USER,
        }
    ]
    sent.clear()
    await director._listening(route(THEO, feel={"laughing": 0.2, "neutral": 0.25}))
    assert sent == []  # Jev is guessing: they stay as they were


def test_a_reaction_needs_someone_to_hear_it() -> None:
    reading = Reading("reply", THEO, "…", USER, {USER: 0.9}, momentum=0.5, react={NOBODY: 0.9})
    assert engine_with({THEO}).after(reading, frozenset({USER})).reaction is None
