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
from room import (
    NOTE,
    NOTE_ACT,
    NOTE_ACT_FOR,
    NOTE_EVENT,
    NOTE_NO_MUSIC,
    USER,
    Reading,
    flower_color,
)
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


def test_nobody_dances_without_music_they_ask_for_a_record_on(director: Director) -> None:
    plan = Plan([Take(JUNO, "addressed", None, USER)], why="addressed", addressed=[JUNO])
    assert director._with_act(plan, route(JUNO, act={"dance": 0.9})) is None
    assert plan.takes[0].note == NOTE_NO_MUSIC
    director.space.music = "hall"
    plan = Plan([Take(JUNO, "addressed", None, USER)], why="addressed", addressed=[JUNO])
    assert director._with_act(plan, route(JUNO, act={"dance": 0.9})) is not None


def test_everyone_asked_dances_but_only_one_bakes_the_cake(director: Director) -> None:
    def everyone() -> Plan:
        takes = [Take(c, "chorus", "everyone", USER) for c in (MAYA, THEO, JUNO)]
        return Plan(takes, together=True, why="chorus", addressed=[MAYA, THEO, JUNO])

    director.space.music = "hall"
    dance = director._with_act(everyone(), route(MAYA, act={"dance": 0.9}))
    assert dance is not None and dance["who"] == [JUNO, MAYA, THEO]
    plan = everyone()
    cook = director._with_act(plan, route(MAYA, act={"cook": 0.9}))
    assert cook is not None and cook["who"] == [MAYA]
    assert [t.act for t in plan.takes] == ["cook", None, None]


def test_what_theyre_asked_to_bring_someone_else_goes_to_them(director: Director) -> None:
    plan = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    act = director._with_act(plan, route(THEO, act={"food": 0.9}, for_={MAYA: 0.8, USER: 0.2}))
    assert act == {"type": "act", "who": [THEO], "action": "food", "to": MAYA}
    assert plan.takes[0].note == NOTE_ACT_FOR["food"].format(to="Maya")
    # For the user, it's as before; and nobody brings themselves anything.
    mine = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    act = director._with_act(mine, route(THEO, act={"food": 0.9}, for_={USER: 0.9}))
    assert act is not None and "to" not in act
    own = Plan([Take(THEO, "addressed", None, USER)], why="addressed", addressed=[THEO])
    act = director._with_act(own, route(THEO, act={"food": 0.9}, for_={THEO: 0.9}))
    assert act is not None and "to" not in act


def test_a_flower_is_the_color_asked_for_or_a_surprise(director: Director) -> None:
    assert flower_color("Could I have a yellow flower?") == "yellow"
    assert flower_color("red or white, I can't decide") is None
    assert flower_color("pick me a flower") is None
    plan = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    reading = route(MAYA, act={"flower": 0.9})
    reading.heard = "Maya, could you pick me a yellow flower?"
    act = director._with_act(plan, reading)
    assert act == {"type": "act", "who": [MAYA], "action": "flower", "color": "yellow"}
    assert "a yellow flower" in (plan.takes[0].note or "")
    surprise = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    reading.heard = "Maya, could you pick me a flower?"
    act = director._with_act(surprise, reading)
    assert act is not None and "color" not in act
    assert "Don't say what color" in (surprise.takes[0].note or "")


def test_asked_to_introduce_the_user_they_lead_the_way_and_wait(director: Director) -> None:
    director.space.update(
        {
            "user": {"area": "conservatory"},
            "characters": {
                MAYA: {"area": "conservatory", "hears": [USER]},
                JUNO: {"area": "music", "hears": []},
            },
        }
    )
    plan = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    reading = route(MAYA, act={"introduce": 0.9}, for_={JUNO: 0.9}, move={"go:music": 0.9})
    assert director._with_move(plan, reading) is None  # the introduction leads the way
    act = director._with_act(plan, reading)
    assert act == {"type": "act", "who": [MAYA], "action": "introduce", "to": JUNO}
    assert plan.takes[0].note == NOTE_ACT_FOR["introduce"].format(to="Juno")
    assert "follow you" in (plan.takes[0].note or "")
    # Juno's right here: no leading the way.
    director.space.update(
        {
            "user": {"area": "music"},
            "characters": {
                MAYA: {"area": "music", "hears": [USER, JUNO]},
                JUNO: {"area": "music", "hears": [USER, MAYA]},
            },
        }
    )
    here = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    director._with_act(here, reading)
    assert "right here" in (here.takes[0].note or "")
    # Jev unsure who, but the user named her.
    named = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    unsure = route(MAYA, act={"introduce": 0.9}, for_={USER: 0.7, JUNO: 0.3})
    unsure.heard = "Maya, can you introduce me to Juno?"
    act = director._with_act(named, unsure)
    assert act is not None and act["to"] == JUNO
    # To whom? They ask.
    whom = Plan([Take(MAYA, "addressed", None, USER)], why="addressed", addressed=[MAYA])
    assert director._with_act(whom, route(MAYA, act={"introduce": 0.9})) is None
    assert whom.takes[0].note == "Ask the person who they'd like to meet."


async def test_together_at_last_they_introduce_the_user(
    director: Director, played: list[Plan]
) -> None:
    await director.event(
        {"kind": "introduce", "who": MAYA, "to": JUNO, "heard_by": [MAYA, JUNO, USER]}
    )
    [plan] = played
    assert plan.takes[0].speaker == MAYA and plan.takes[0].to == JUNO
    assert "Introduce them to each other" in (plan.takes[0].note or "")
    assert JUNO in director.space.talked  # and now the user has met her


def test_doing_what_the_user_asked_their_own_lines_dont_send_them_off(
    director: Director,
) -> None:
    take = Take(MAYA, "addressed", None, USER)
    assert director._free(MAYA, take)
    director.space.update(
        {"characters": {MAYA: {"area": "hall", "hears": [USER], "intent": "follow"}}}
    )
    assert not director._free(MAYA, take)  # following the user
    director.space.update(
        {"characters": {MAYA: {"area": "hall", "hears": [USER], "doing": "watering the plants"}}}
    )
    assert not director._free(MAYA, take)  # busy already
    director.space.update({"characters": {MAYA: {"area": "hall", "hears": [USER]}}})
    assert director._free(MAYA, take)
    assert not director._free(MAYA, Take(MAYA, "addressed", None, USER, moving=True))


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


async def test_whoever_is_handed_something_says_so_to_whoever_gave_it(
    director: Director, played: list[Plan]
) -> None:
    await director.event(
        {
            "kind": "handed",
            "who": THEO,
            "to": MAYA,
            "item": "a slice of cake",
            "heard_by": [THEO, MAYA],
        }
    )
    [plan] = played
    assert [t.speaker for t in plan.takes] == [MAYA] and plan.takes[0].to == THEO
    assert plan.takes[0].note == NOTE_EVENT["received"].format(who="Theo", item="a slice of cake")
    [note] = [ln for ln in director.transcript.lines if ln.speaker == NOTE]
    assert note.text == "Theo handed Maya a slice of cake."


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
    await director._news()
    assert played == [] and director._events == []
    director._events.append((time.monotonic(), plan))
    await director._news()
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
    await director._news()
    assert [p.why for p in played] == ["visit"]
    monkeypatch.setattr(Director, "busy", property(lambda self: True))
    await director.event({"kind": "visit", "who": JUNO, "to": THEO, "heard_by": [JUNO, THEO]})
    await director.event({"kind": "bell", "who": USER, "heard_by": [MAYA, THEO, JUNO]})
    assert [p.why for _, p in director._events] == ["bell"]
