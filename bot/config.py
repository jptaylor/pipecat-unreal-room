"""Settings from `bot/.env` (explicitly set variables win), the cast from `bot/characters.json`
(the game reads it too), and every tunable the room uses."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv

HERE = Path(__file__).resolve().parent

# --- Models -----------------------------------------------------------------------------------

JEV_MODEL = "jev-1.13.0"  # the floors below were tried on this version
DEEPGRAM_MODEL = "flux-general-en"  # Flux: transcription with its own end-of-turn detection
OPENAI_MODEL = "gpt-4.1-mini"  # the characters' LLM, unless PhoneLLM is set up (see Settings)

# --- Workers ----------------------------------------------------------------------------------

ROOM = "room"  # the main worker: transport, STT, the director; each character is a worker too

# --- Jev ----------------------------------------------------------------------------------------

# Lines of the conversation Jev is shown with each question: enough to keep the introductions in
# view, so "so who liked blue?" can find who said it.
HISTORY_LINES = 30
# Whoever the user spoke to last counts this many times as likely before Jev's reading of the new
# words (1 turns it off). Kept light: Jev already sees who that was, so this only settles
# near-ties, and a name, a question about what someone said, or a correction always wins.
RECENCY_WEIGHT = 1.2
INCLUDED_FLOOR = 0.5  # a group turn: how sure Jev must be that a character is one of those asked
CHORUS_FLOOR = 0.5  # ...and that they can all answer at once, rather than in turn
ONLY_IF_FLOOR = 0.5  # ...and that only those it's true for should ("say yes if…")
TRUE_OF_FLOOR = 0.5  # ...and, for each, that it's true of them
# How sure Jev must be the user means "carry on" or "quiet now", not "answer me": a quiet table
# when they wanted an answer is the worse mistake.
INTENT_FLOOR = 0.7
# How sure Jev must be that the user asked whoever they're talking to to move ("follow me",
# "wait here", "come over here"): someone walking off when they were only asked a question is
# worse than staying put.
MOVE_FLOOR = 0.6
# ...or to do something (dance, play the piano, bring them cake): the same, and more so for a
# character setting about something of their own accord, as they say a line.
ACT_FLOOR = 0.6
DOING_FLOOR = 0.75
# Something that happened in the house (a gift, the bell, music) is reacted to this soon, or not
# at all: by the time the room is quiet again, it's old news.
EVENT_STALE_S = 20.0
REPLY_FLOOR = 0.5  # after a line: how sure Jev must be that it's for this character to answer,
TABLE_FLOOR = 0.8  # ...unless it's this sure it's for someone at the table: the likeliest answers
REACT_FLOOR = 0.6  # ...and that someone reacts out loud (a laugh, a groan) as it ends
# How the characters feel and what they do with their hands, as Jev reads each line (the WoW
# companion's portrait does the same with its face): below these, Jev is guessing, and they stay
# as they were.
MOOD_FLOOR = 0.3
GESTURE_FLOOR = 0.4
LISTENER_MOOD_STRENGTH = 0.6  # those listening show it less than whoever's speaking
MAX_CAST = 8
JEV_TIMEOUT_S = 3.0
JEV_CACHE_SIZE = 256  # questions asked while the user spoke make the final one free
ROUTE_WAIT_S = 1.5  # a route never waits longer than this on Jev: then the last speaker answers
# Jev reads the user's words while they're still speaking. If that read was this sure who they
# were talking to (one character), that character's line is started as soon as the turn ends,
# held until the final read agrees, instead of after it: Jev's ~250 ms off the critical path.
SPECULATE_FLOOR = 0.8

# --- The user's turn ----------------------------------------------------------------------------

# When it ends is Deepgram Flux's call, on its defaults (services.py). The room only makes sure
# nobody talks over the user: whenever their voice starts again after their turn ended, it waits
# for those words to become a turn (the transcript trails the voice), up to MORE_MAX_S, or for
# TRANSCRIPT_LAG_S after the voice stops without one (a cough). A turn that began within
# MERGE_GAP_S of the last one ending, with nothing heard in between, is read as one turn.
MORE_MAX_S = 3.0
TRANSCRIPT_LAG_S = 0.8
MERGE_GAP_S = 2.0
MIN_WORDS_TO_INTERRUPT = 2  # while anyone is talking: "hang on" takes the floor, "haha" doesn't

# --- How long the characters talk among themselves --------------------------------------------

# There is no turn count. Each line is scored by Jev for momentum (how much it asks to be
# answered, 0 to 1), and the room tires as it talks: its energy halves every ENERGY_HALF_LIFE_S
# seconds of talk since the user last spoke (estimated from the words). A line is answered while
# momentum × energy is at least CONTINUE_FLOOR; once it falls under, the next speaker is asked to
# land it (a wrap-up), and then the floor is the user's. A line with no momentum to speak of has
# landed by itself, and ends the exchange at once.
ENERGY_HALF_LIFE_S = 50.0
CONTINUE_FLOOR = 0.3
LANDED_MOMENTUM = 0.2
WORDS_PER_S = 2.7  # Cartesia's pace, near enough: how long a line takes to say
TURN_GAP_S = 0.25  # the beat between one speaker finishing and the next starting
MAX_TALK_S = 180.0  # a backstop only: past this, the next line is a wrap-up whatever Jev reads
REACT_COOLDOWN = 2  # lines that must pass between two reactions
CHORUS_STAGGER_S = 1.0  # a chorus starts spread over this long from the first voice, as people do

# When the floor is the user's and they say nothing, someone near them picks something up after
# each of these waits in turn; each of those exchanges starts LULL_HEAD_START_S more tired than
# the last, and once the waits run out they stay quiet until the user speaks.
LULL_WAITS_S = (10.0, 20.0, 35.0)
LULL_HEAD_START_S = 20.0

# --- Turns over the bus -------------------------------------------------------------------------

SPEAK_JOB = "speak"
SPEAK_TIMEOUT_S = 45.0  # writing and voicing one line, however long it waits for its turn
WRITE_TIMEOUT_S = 15.0  # a character that hasn't written its line by then says nothing
VOICE_TIMEOUT_S = 15.0  # ...nor heard its TTS finish: the line is taken as voiced
# A line that has stopped playing but whose character hasn't said it's all sent is taken as
# finished after this long, so nobody waiting on it is held up for good.
FINISH_GRACE_S = 2.0

# --- The voices, to the game --------------------------------------------------------------------

# The game plays each character's voice from their head: the output sends them all in one stream,
# a channel each (mixer.py), paced at the speed they play, this far ahead of it, and holds a
# character's TTS once this much of their voice is waiting to be sent.
VOICE_LEAD_S = 0.12
VOICE_BLOCK_S = 0.02
VOICE_QUEUE_S = 0.1

# --- The characters' LLM ----------------------------------------------------------------------

LLM_TOKENS = 200
LLM_HISTORY_LINES = 60  # the most of the conversation a character is shown, in lines
LLM_TEMPERATURE = 0.0  # PhoneLLM must run at temperature 0: never change this
# Also counts the prompt's tokens (vLLM): without it, a character at temperature 0 soon answers
# with a line it already said, word for word.
LLM_REPETITION_PENALTY = 1.1  # the default; LLM_REPETITION_PENALTY in .env overrides (1.0: off)
LLM_WARM_TIMEOUT_S = 240.0  # the hosted endpoint scales to zero; a cold start takes minutes


@dataclass(frozen=True)
class Character:
    id: str
    name: str
    role: str
    tagline: str
    voice: str  # Cartesia voice id
    topics: str  # what they know best: Jev routes a question with no name by it
    colour: str  # their favourite colour (and their color in the game)
    home: str = ""  # the area of the house they live in, e.g. "kitchen"...
    place: str = ""  # ...and what it's called, e.g. "the kitchen"

    def brief(self) -> str:
        return f"{self.name}, the {self.role.lower()}: {self.tagline}"


def load_cast(source: Path | None = None) -> tuple[Character, ...]:
    """The characters, from `characters.json`, beside the code (the game reads the same file),
    or from `source`."""
    fields = ("id", "name", "role", "tagline", "voice", "topics", "colour")
    cast = tuple(
        Character(
            **{k: entry[k] for k in fields},
            home=entry.get("home", ""),
            place=entry.get("place", ""),
        )
        for entry in json.loads((source or HERE / "characters.json").read_text())
    )
    ids = [c.id for c in cast]
    if not 2 <= len(cast) <= MAX_CAST or len(set(ids)) != len(ids):
        raise ValueError(f"characters.json must list 2 to {MAX_CAST} characters, ids distinct")
    return cast


def load_environment() -> None:
    load_dotenv(HERE / ".env", override=False)


def require(name: str) -> str:
    value = (os.getenv(name) or "").strip()
    if not value:
        raise RuntimeError(f"set {name} in bot/.env")
    return value


@dataclass(frozen=True)
class Settings:
    """The services' keys. The characters speak with PhoneLLM where PHONELLM_API_KEY,
    PHONELLM_BASE_URL and LLM_MODEL are set, as the kitchen table does, and with OpenAI
    otherwise (OPENAI_API_KEY, and OPENAI_MODEL to choose the model)."""

    llm_api_key: str
    llm_base_url: str | None
    llm_model: str
    phonellm: bool
    jev_api_key: str
    jev_model: str
    cartesia_api_key: str
    deepgram_api_key: str
    llm_repetition_penalty: float = LLM_REPETITION_PENALTY

    @classmethod
    def from_env(cls) -> Settings:
        phonellm = all(os.getenv(k) for k in ("PHONELLM_API_KEY", "PHONELLM_BASE_URL", "LLM_MODEL"))
        if phonellm:
            key, url, model = (
                require("PHONELLM_API_KEY"),
                require("PHONELLM_BASE_URL"),
                require("LLM_MODEL"),
            )
        else:
            key, url = require("OPENAI_API_KEY"), None
            model = (os.getenv("OPENAI_MODEL") or OPENAI_MODEL).strip()
        return cls(
            llm_api_key=key,
            llm_base_url=url,
            llm_model=model,
            phonellm=phonellm,
            jev_api_key=require("JEV_API_KEY"),
            jev_model=(os.getenv("JEV_MODEL") or JEV_MODEL).strip(),
            cartesia_api_key=require("CARTESIA_API_KEY"),
            deepgram_api_key=require("DEEPGRAM_API_KEY"),
            llm_repetition_penalty=float(
                os.getenv("LLM_REPETITION_PENALTY") or LLM_REPETITION_PENALTY
            ),
        )
