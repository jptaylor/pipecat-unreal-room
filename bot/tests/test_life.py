"""Life in the house, offline: the user asks someone to do something and they do it, the
characters set about things of their own accord, and things that happen in the house (a gift,
the bell, the music) are known by whoever saw or heard them and said something about by whoever
they're most about."""

from __future__ import annotations

import time
from typing import Any

import pytest

from config import ACT_FLOOR, DOING_FLOOR, load_cast
from director import Director
from engine import Plan, Take
from room import NOTE, NOTE_ACT, NOTE_EVENT, USER, Reading
from space import Space

CAST = load_cast()
MAYA, THEO, JUNO = (c.id for c in CAST)


def route(choice: str, **fields: Any) -> Reading:
    reading = Reading("route", USER, "…", choice, {choice: 0.9})
    for name, value in fields.items():
        setattr(reading, name, value)
    return reading


@pytest.fixture
def director() -> Director:
    return Director(CAST, referee=None)  # type: ignore[arg-type]


@pytest.fixture
def played(director: Director, monkeypatch) -> list[Plan]:
    """The plans the director plays, instead of playing them."""
    plans: list[Plan] = []

    async def play(plan: Plan, *args: Any, **kwargs: Any) -> None:
        plans.append(plan)

    monkeypatch.setattr(director, "play", play)
    return plans


# --- Asked to do something ----------------------------------------------------------------------


def test_doing_something_is_only_what_jev_is_sure_of() -> None:
    assert route(JUNO, act={"play": 0.8, "none": 0.2}).wants_act(ACT_FLOOR) == "play"
    assert route(JUNO, act={"play": 0.4, "none": 0.6}).wants_act(ACT_FLOOR) is None
    assert route(JUNO, act={"none": 0.9}).wants_act(ACT_FLOOR) is None
    reply = Reading("reply", THEO, "…", USER, doing={"cook": 0.9})
    assert reply.does(DOING_FLOOR) == "cook"
    assert Reading("reply", THEO, "…", USER, doing={"cook": 0.6}).does(DOING_FLOOR) is None


def test_whoever_is_asked_to_do_something_is_told_so_and_the_game_has_them_do_it(
    director: Director,
) -> None:
    plan = Plan([Take(JUNO, "addressed", None, USER)], why="addressed", addressed=[JUNO])
    act = director._with_act(plan, route(JUNO, act={"play": 0.9}))
    assert act == {"type": "act", "who": [JUNO], "action": "play"}
    assert plan.takes[0].note == NOTE_ACT["play"] and plan.takes[0].act == "play"


def test_everyone_asked_dances_but_only_one_bakes_the_cake(director: Director) -> None:
    def everyone() -> Plan:
        takes = [Take(c, "chorus", "everyone", USER) for c in (MAYA, THEO, JUNO)]
        return Plan(takes, together=True, why="chorus", addressed=[MAYA, THEO, JUNO])

    dance = director._with_act(everyone(), route(MAYA, act={"dance": 0.9}))
    assert dance is not None and dance["who"] == [JUNO, MAYA, THEO]
    plan = everyone()
    cook = director._with_act(plan, route(MAYA, act={"cook": 0.9}))
    assert cook is not None and cook["who"] == [MAYA]
    assert [t.act for t in plan.takes] == ["cook", None, None]


def test_nobody_does_anything_on_a_turn_they_didnt_hear(director: Director) -> None:
    plan = Plan([Take(THEO, "not_heard", "…", USER)], why="not_heard", addressed=[MAYA])
    assert director._with_act(plan, route(MAYA, act={"flower": 0.9})) is None
    assert plan.takes[0].act is None


# --- What happens in the house -------------------------------------------------------------------


async def test_whoever_hands_the_person_something_says_so(
    director: Director, played: list[Plan]
) -> None:
    await director.event(
        {"kind": "handed", "who": THEO, "item": "a slice of cake", "heard_by": [THEO, USER]}
    )
    [plan] = played
    assert [t.speaker for t in plan.takes] == [THEO]
    assert plan.takes[0].note == NOTE_EVENT["handed"].format(item="a slice of cake")
    # Those who saw it know; those who didn't, don't.
    [note] = [ln for ln in director.transcript.lines if ln.speaker == NOTE]
    assert note.text == "Theo handed the person a slice of cake."
    assert note.heard(THEO) and not note.heard(MAYA)


async def test_the_bell_brings_everyone_and_they_all_say_something(
    director: Director, played: list[Plan]
) -> None:
    await director.event({"kind": "bell", "who": USER, "heard_by": [MAYA, THEO, JUNO]})
    [plan] = played
    assert plan.together and sorted(t.speaker for t in plan.takes) == [JUNO, MAYA, THEO]


async def test_the_musician_is_the_one_to_say_something_about_the_music(
    director: Director, played: list[Plan]
) -> None:
    await director.event({"kind": "music", "on": True, "who": USER, "heard_by": [MAYA, JUNO]})
    [plan] = played
    assert plan.why == "music_on" and plan.takes[0].speaker == JUNO


async def test_a_visit_is_a_word_with_whoever_is_visited(
    director: Director, played: list[Plan]
) -> None:
    await director.event(
        {"kind": "visit", "who": JUNO, "to": THEO, "place": "the kitchen", "heard_by": [THEO]}
    )
    [plan] = played
    assert plan.takes[0].speaker == JUNO and plan.takes[0].to == THEO
    assert "Theo" in (plan.takes[0].note or "")


async def test_what_nobody_saw_is_only_known_and_not_remarked_on(
    director: Director, played: list[Plan]
) -> None:
    await director.event({"kind": "wish", "who": USER, "heard_by": [USER]})
    assert played == []
    [note] = [ln for ln in director.transcript.lines if ln.speaker == NOTE]
    assert not any(note.heard(c) for c in (MAYA, THEO, JUNO))


async def test_whats_happened_waits_until_nobody_is_talking_and_goes_stale(
    director: Director, played: list[Plan], monkeypatch
) -> None:
    monkeypatch.setattr(Director, "busy", property(lambda self: True))
    await director.event({"kind": "baked", "who": THEO, "heard_by": [THEO]})
    assert played == [] and len(director._events) == 1
    monkeypatch.setattr(Director, "busy", property(lambda self: False))
    at, plan = director._events[0]
    director._events[0] = (at - 60.0, plan)  # long ago: it's not news any more
    await director._greet()
    assert played == [] and director._events == []
    director._events.append((time.monotonic(), plan))
    await director._greet()
    assert played == [plan]


# --- What they know about the house ----------------------------------------------------------


def test_each_knows_what_they_and_the_house_are_up_to() -> None:
    space = Space(CAST)
    space.world(
        {"areas": [{"id": "kitchen", "name": "the kitchen"}, {"id": "hall", "name": "the hall"}]}
    )
    space.update(
        {
            "user": {"area": "kitchen"},
            "characters": {
                THEO: {"area": "kitchen", "hears": [USER], "doing": "baking a cake", "holding": ""},
                MAYA: {"area": "hall", "hears": [], "doing": "", "holding": "a flower"},
            },
            "world": {"music": "hall", "cake": True, "holding": "a slice of cake"},
        }
    )
    theo = space.situation(THEO)
    assert "You're baking a cake." in theo
    assert "The person is holding a slice of cake." in theo
    assert "Music is playing in the hall." in theo
    maya = space.situation(MAYA)
    assert "You're holding a flower." in maya
    assert "slice of cake" not in maya  # she can't see the person from there
    assert space.for_jev()["doing"] == {"Theo": "baking a cake"}


async def test_the_latest_news_comes_first_and_the_bell_drowns_out_the_rest(
    director: Director, played: list[Plan], monkeypatch
) -> None:
    monkeypatch.setattr(Director, "busy", property(lambda self: True))
    await director.event({"kind": "baked", "who": THEO, "heard_by": [THEO]})
    await director.event({"kind": "visit", "who": MAYA, "to": JUNO, "heard_by": [MAYA, JUNO]})
    monkeypatch.setattr(Director, "busy", property(lambda self: False))
    await director._greet()
    assert [p.why for p in played] == ["visit"]
    monkeypatch.setattr(Director, "busy", property(lambda self: True))
    await director.event({"kind": "visit", "who": JUNO, "to": THEO, "heard_by": [JUNO, THEO]})
    await director.event({"kind": "bell", "who": USER, "heard_by": [MAYA, THEO, JUNO]})
    assert [p.why for _, p in director._events] == ["bell"]
