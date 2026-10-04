"""The director's handling of the user's turns, offline: a turn that carries on one nobody has
answered yet, and waiting while the user is still going."""

from __future__ import annotations

import asyncio
import time
from pathlib import Path

import pytest

from config import load_cast
from director import Director, same_plan
from engine import Plan, Take
from room import NOTE, USER

# The kitchen table's six, whose routing these tests were written for.
CAST = load_cast(Path(__file__).parent / "characters.json")
IDS = [c.id for c in CAST]


@pytest.fixture
def director() -> Director:
    return Director(CAST, referee=None)  # type: ignore[arg-type]


def test_a_turn_that_carries_on_one_nobody_answered_is_read_as_one(director: Director) -> None:
    line, first_id, _, merged = director._user_line("Felix.", ended=100.0)
    assert not merged
    director.transcript.add(NOTE, "an aside of the route that's void now")
    director._voice_starts.append(100.6)  # they went on 0.6 s after the first words ended
    same, same_id, history, merged = director._user_line("Can you fix my bike?", ended=102.0)
    assert merged and same is line and same_id == first_id
    assert line.text == "Felix. Can you fix my bike?"
    assert [ln.speaker for ln in director.transcript.lines] == [USER]  # the aside is gone
    assert history == []


def test_a_turn_after_someone_was_heard_is_a_new_turn(director: Director) -> None:
    director._user_line("Felix.", ended=100.0)
    director.transcript.add(IDS[4], "Yes?")
    director._voice_starts.append(100.6)
    _, _, _, merged = director._user_line("Can you fix my bike?", ended=102.0)
    assert not merged and len(director.transcript.lines) == 3


def test_a_turn_that_went_on_after_a_second_pause_still_counts_from_the_first(
    director: Director,
) -> None:
    director._user_line("Okay.", ended=100.0)
    director._voice_starts.extend([100.7, 103.0])  # "So what I want is" … "for Theo to cook"
    _, _, _, merged = director._user_line("So what I want is for Theo to cook.", ended=104.0)
    assert merged


def test_a_turn_long_after_the_last_is_a_new_turn(director: Director) -> None:
    director._user_line("Felix.", ended=100.0)
    director._voice_starts.append(108.0)
    _, _, _, merged = director._user_line("Can you fix my bike?", ended=110.0)
    assert not merged


async def test_waiting_for_more_ends_when_a_new_turn_takes_the_floor(director: Director) -> None:
    epoch, ended = director._epoch, time.monotonic()
    await asyncio.sleep(0.01)
    await director.voice(True)  # they started again after their turn's words ended
    waiting = asyncio.create_task(director._more(epoch, ended))
    await asyncio.sleep(0.05)
    await director.take_floor()  # those words became a turn
    assert await asyncio.wait_for(waiting, 1.0) is True


async def test_no_waiting_for_more_when_they_dont_go_on(director: Director) -> None:
    started = time.monotonic()
    assert await director._more(director._epoch, started) is False
    assert time.monotonic() - started < 0.1


async def test_while_their_voice_goes_on_the_table_waits(director: Director, monkeypatch) -> None:
    monkeypatch.setattr("director.MORE_MAX_S", 0.3)
    ended = time.monotonic()
    await asyncio.sleep(0.01)
    await director.voice(True)  # they started again after their turn's words ended
    started = time.monotonic()
    assert await director._more(director._epoch, ended) is False
    assert time.monotonic() - started >= 0.25  # waited for those words, up to the limit


def test_a_guess_stands_only_if_the_final_read_plans_the_same_lines() -> None:
    def one(who: str) -> Plan:
        return Plan([Take(who, "addressed", None)], why="addressed")

    def chorus(*who: str) -> Plan:
        return Plan([Take(w, "chorus", "everyone") for w in who], True, "chorus")

    assert same_plan(one(IDS[0]), one(IDS[0]))
    assert not same_plan(one(IDS[0]), one(IDS[1]))
    assert same_plan(chorus(IDS[0], IDS[1]), chorus(IDS[1], IDS[0]))  # said at once: any order
    assert not same_plan(chorus(IDS[0], IDS[1]), chorus(IDS[0]))
    assert not same_plan(one(IDS[0]), chorus(IDS[0]))


def test_the_same_words_transcribed_in_two_parts_are_one_turn(director: Director) -> None:
    director._voice_starts.append(99.0)
    director._voice_ended = 100.0
    line, _, _, _ = director._user_line("Maya,", ended=100.0)
    # No new words since (the voice never started again): the rest of the same utterance.
    _, _, _, merged = director._user_line("what are you growing at the moment?", ended=100.0)
    assert merged and line.text == "Maya, what are you growing at the moment?"
