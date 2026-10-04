"""The room: one conversation between the user and a cast of characters around a house, and
Jev's read of it.

`Transcript` is the conversation as it happened, who said what, and who heard it: in the house,
only those within earshot hear a line (`space.py`). Each character's LLM sees it from its own
seat (`Transcript.view`): its own lines are its replies, and everyone else's that it heard are
user turns marked with who spoke. What it didn't hear, it never sees.

`Referee` asks Jev three kinds of question, each set in one request:

- After the user speaks (`addressee`, asked on their partial transcripts too, so the answer is
  usually cached by the time they stop): who they're talking to, one character or a group; for a
  group, who's included and whether they can all answer at once (a chorus); what the user
  wants as the table's host: an answer, for the table to carry on, or quiet; whether they're
  asking whoever they're talking to to move (follow them, wait, come over, go somewhere); and
  how those listening take it, which their posture shows as they listen.
- After a character's line (`reply`, asked as soon as the line is written, while it plays): who
  answers it, if anyone; how much momentum it has; whether someone reacts out loud; and the
  mood and gesture the speaker says it with, and how the others take it, for the game to pose
  them (as the WoW companion picks its portrait's face, line by line).
- When the table has gone quiet (`lull`): who would naturally break the silence.

`engine.py` decides what to do with the answers.
"""

from __future__ import annotations

import asyncio
import json
import re
import time
from collections import OrderedDict
from collections.abc import Sequence
from dataclasses import dataclass, field, replace
from difflib import SequenceMatcher
from typing import Any

from loguru import logger
from pipecat.classifiers.base_classifier import (
    BaseClassifier,
    ChoiceQuestion,
    ChoiceResult,
    ClassifierQuestion,
    ClassifierResult,
    ScoreQuestion,
    ScoreResult,
    YesNoQuestion,
    YesNoResult,
)

from config import (
    HISTORY_LINES,
    INCLUDED_FLOOR,
    JEV_CACHE_SIZE,
    LLM_HISTORY_LINES,
    WORDS_PER_S,
    Character,
)

USER = "user"
NOTE = "note"  # not a speaker: a note in the transcript every character sees, never said
GROUP = "group"
NOBODY = "nobody"
ADDRESSEE = "addressee"  # the user's line: who it's for (a choice)...
INCLUDED = "with:"  # ...prefix of the per-character yes/no questions (is each one included?)
CHORUS = "chorus"  # ...can a group answer all at once?
ONLY_IF = "only_if"  # ...and should only those it's true for answer ("say yes if…")?
TRUE_OF = "true:"  # ...prefix of the per-character yes/no questions: is it true of each?
INTENT = "intent"  # ...and what the user wants as host
MOVE = "move"  # ...whether they ask whoever they're talking to to move
ACT = "act"  # ...or to do something: dance, play the piano, bring them cake…
FOR = "for"  # ...and, for something to bring or hand over, who it's for
FEEL = "feel"  # ...and how those listening take it
ANSWER, CARRY_ON, HUSH = "answer", "carry_on", "hush"
STAY, COME, FOLLOW, WAIT, HOME = "stay", "come", "follow", "wait", "home"
GO = "go:"  # prefix of the moves to an area, e.g. "go:kitchen"
MOOD = "mood"  # a character's line: the mood they say it in...
GESTURE = "gesture"  # ...the gesture they make...
TAKEN = "taken"  # ...and how the others hearing it take it
DOING = "doing"  # ...and whether the speaker sets about doing something as they say it
NO_ACT = "none"
REPLY = "reply:"  # a character's line: one set of questions per speaker...
NEXT = "next"  # ...who answers it, or the user
MOMENTUM = "momentum"  # ...how much it asks to be answered
REACT = "react"  # ...who reacts out loud as it ends, if anyone
LULL = "lull"  # the table has gone quiet: who breaks the silence

# What a character writes to say nothing at all (never voiced).
SILENT = "[silent]"

# The moods a character's body shows, and the gestures they make, which the game poses them in
# (RoomRig.cpp). Each is described for Jev.
MOODS = {
    "neutral": "calm or matter-of-fact: an ordinary question, answer, request or remark",
    "happy": "warm, friendly or pleased: a greeting, a kind word, good news, an invitation",
    "laughing": "amused: a joke, a pun, a tease, something silly",
    "excited": "thrilled or eager: a treat, a plan, a party, something they'd love",
    "proud": "proud or flattered: praise, boasting, an achievement",
    "surprised": "astonished: unexpected news, a strange question, a revelation",
    "thinking": "pondering or unsure: a hard question, a riddle, a choice, trying to remember",
    "worried": "concerned or nervous: danger, trouble, bad news coming, a risky plan",
    "sad": "sad, sorry or sympathetic: a loss, a disappointment, loneliness",
    "angry": "annoyed, offended or indignant: rudeness, an insult, something unfair",
    "bored": "unimpressed or weary: something dull, repetitive or long-winded",
    "doubtful": "skeptical: a dubious claim, a tall tale, something they'd rather not do",
}
# What a character can be asked to do, or set about doing, in the house (the game does it:
# RoomStageLife.cpp). Each is described for Jev, as asked of them by the user.
ACTS = {
    NO_ACT: (
        "nothing to do now: a question, a remark, a greeting, or talk about doing something "
        "that doesn't ask for it now"
    ),
    "dance": "dance: 'let's dance', 'dance with me', 'show me your moves'",
    "play": "play the piano in the music room: 'play something', 'play us a song', 'a tune, please'",
    "music_on": (
        "put a record on the gramophone in the hall: 'put some music on', 'let's have some "
        "music', 'party time'"
    ),
    "music_off": "stop the music: 'turn the music off', 'stop playing', 'that's enough music'",
    "cook": "cook or bake something: 'bake a cake', 'make something to eat', 'cook dinner'",
    "food": (
        "bring someone something to eat, the user or someone else: 'can I have some cake?', "
        "'I'm hungry', 'got anything to eat?', 'a tomato, please', 'bring Maya a slice'"
    ),
    "flower": (
        "pick someone a flower, the user or someone else: 'can I have a flower?', 'pick me a "
        "flower', 'give Juno a flower'"
    ),
    "water": "water the plants: 'water the plants', 'make the plants grow'",
    "stop": "stop what they're doing: 'stop dancing', 'you can stop playing now', 'stop cooking'",
    "hand": (
        "hand what they're holding to someone, the user or someone else: 'give me that', 'can I "
        "have it?', 'pass it here', 'give that to Theo'"
    ),
    "introduce": (
        "take the user to meet someone, and introduce them: 'can you introduce me to Juno?', "
        "'I'd like to meet Theo', 'take me to meet your friends'"
    ),
}
# ...and as they set about it themselves, as they say a line.
DOINGS = {
    NO_ACT: (
        "nothing: as with most lines; suggesting, offering or talking about doing something "
        "isn't starting to do it"
    ),
    "dance": "starts dancing: 'come on, let's dance!', 'I can't help it, I have to dance'",
    "play": "goes to play the piano: 'let me play you something', 'I'll play you a tune'",
    "music_on": "goes to put a record on: 'let me put a record on', 'music, maestro!'",
    "music_off": "goes to turn the music off: 'I'll turn it off'",
    "cook": "starts cooking or baking: 'I'll whip up a cake', 'let me get something on the stove'",
    "food": "goes to bring the user something to eat: 'I'll get you a slice', 'here, have a tomato'",
    "flower": "goes to pick the user a flower: 'let me pick you one', 'a flower for you'",
    "water": "goes to water the plants: 'time to water my babies'",
    "stop": "stops what they're doing: 'alright, I'll stop'",
    "hand": "hands the user what they're holding: 'here, take it'",
}
# The acts only one character does, even asked of several (one cake, one pianist).
SINGULAR_ACTS = {
    "play",
    "music_on",
    "music_off",
    "cook",
    "food",
    "flower",
    "water",
    "hand",
    "introduce",
}

GESTURES = {
    "none": "no particular gesture, as with most lines",
    "nod": "agreeing, saying yes, confirming",
    "shake": "disagreeing, saying no, refusing",
    "shrug": "not knowing or not minding: 'who knows?', 'whatever you like'",
    "wave": "a greeting or a goodbye: 'hi!', 'bye!', 'over here!'",
    "point": "pointing someone or something out: 'you!', 'that's Theo', 'over there'",
    "bow": "thanking someone formally, being modest, a mock bow",
    "clap": "celebrating, applauding, delighted",
    "facepalm": "embarrassed or exasperated: 'oh no', 'I can't believe it'",
    "laugh": "laughing out loud",
    "beckon": "inviting someone over, or to follow: 'come here', 'this way', 'follow me'",
}


def is_silent(text: str) -> bool:
    return normalize(text).lower().startswith(SILENT)


def could_be_silent(text: str) -> bool:
    """Whether a line still being written might yet turn out to be SILENT."""
    so_far = normalize(text).lower()
    return SILENT.startswith(so_far) or so_far.startswith(SILENT)


# How a line was said: on its own turn, in a chorus with others, or as a quick reaction.
SAID, TOGETHER, REACTION = "said", "together", "reaction"


def normalize(text: str) -> str:
    return " ".join(text.split())


def names(labels: Sequence[str]) -> str:
    """'Maya', 'Maya and Theo', 'Maya, Theo and Juno'."""
    if len(labels) <= 1:
        return "".join(labels)
    return f"{', '.join(labels[:-1])} and {labels[-1]}"


def seconds(text: str) -> float:
    """About how long a line takes to say."""
    return len(text.split()) / WORDS_PER_S


@dataclass(eq=False)
class Line:
    speaker: str  # USER or a character id
    text: str
    interrupted: bool = False
    how: str = SAID  # SAID, TOGETHER (a chorus) or REACTION
    chorus: int | None = None  # the lines of one chorus share this
    # Who heard it: character ids, and USER for a character's line the user heard. None:
    # everyone (before the game has said who's where).
    heard_by: frozenset[str] | None = None
    aside: bool = False  # a route's note on how it was taken, void if the turn goes on

    def heard(self, character: str) -> bool:
        return self.speaker == character or self.heard_by is None or character in self.heard_by

    @property
    def by_character(self) -> bool:
        return self.speaker not in (USER, NOTE)


class Transcript:
    """Who said what, in order, and how each character sees it."""

    def __init__(self, cast: Sequence[Character]) -> None:
        self.cast = {c.id: c for c in cast}
        self.lines: list[Line] = []
        # Where everyone is in the house, for Jev (set by the director).
        self.where: dict[str, Any] = {}

    def add(
        self,
        speaker: str,
        text: str,
        *,
        interrupted: bool = False,
        how: str = SAID,
        chorus: int | None = None,
        heard_by: frozenset[str] | None = None,
    ) -> Line:
        line = Line(speaker, normalize(text), interrupted, how, chorus, heard_by)
        self.lines.append(line)
        return line

    def remove(self, line: Line) -> None:
        """A line written but never heard (the user cut in first) is taken back out."""
        if line in self.lines:
            self.lines.remove(line)

    def label(self, speaker: str) -> str:
        if speaker in (USER, NOTE):
            return speaker.capitalize()
        return self.cast[speaker].name

    def since_user(self) -> list[Line]:
        """The characters' lines since the user last spoke."""
        lines: list[Line] = []
        for line in reversed(self.lines):
            if line.speaker == USER:
                break
            if line.by_character:
                lines.append(line)
        return lines[::-1]

    def last_character(self) -> str | None:
        return next((ln.speaker for ln in reversed(self.lines) if ln.by_character), None)

    @staticmethod
    def talked(lines: Sequence[Line]) -> float:
        """About how long `lines` took to say: a chorus is as long as its longest line, since
        they're said at once, and a reaction is said over the end of a line."""
        total, chorus = 0.0, {}
        for line in lines:
            if line.how == REACTION:
                continue
            if line.chorus is not None:
                chorus[line.chorus] = max(chorus.get(line.chorus, 0.0), seconds(line.text))
            else:
                total += seconds(line.text)
        return total + sum(chorus.values())

    def view(
        self, me: str, note: str | None = None, limit: int = LLM_HISTORY_LINES
    ) -> list[dict[str, str]]:
        """The conversation as `me`'s LLM sees it (its last `limit` lines of what it heard): its
        own lines as assistant turns, everyone else's as user turns marked `[User]` or `[Name]`,
        and the moment's note last."""
        messages: list[dict[str, str]] = []

        def say(role: str, content: str) -> None:
            if messages and messages[-1]["role"] == role:
                messages[-1]["content"] += "\n" + content
            else:
                messages.append({"role": role, "content": content})

        for line in [ln for ln in self.lines if ln.heard(me)][-limit:]:
            if line.speaker == NOTE:
                say("user", f"[Note: {line.text}]")
            elif line.speaker == me:
                say("assistant", line.text + ("…" if line.interrupted else ""))
            else:
                cut = " (cut off)" if line.interrupted else ""
                say("user", f"[{self.label(line.speaker)}] {line.text}{cut}")
        if messages and messages[0]["role"] == "assistant":
            messages.insert(0, {"role": "user", "content": f"[Note: {NOTE_JOINED}]"})
        if note:
            say("user", f"[Note: {note}]")
        elif not messages or messages[-1]["role"] == "assistant":
            say("user", f"[Note: {NOTE_CARRY_ON}]")
        return messages

    def for_jev(
        self,
        history: Sequence[Line],
        latest: Line,
        last_addressed: Sequence[str] = (),
        quiet_for: float | None = None,
    ) -> dict[str, Any]:
        """Jev's state: who is in the room, the recent conversation, who the user spoke to last
        (for a user's line), and the line in question (or how long the table has been quiet)."""
        cast = list(self.cast.values())
        state: dict[str, Any] = {
            "setting": (
                f"A spoken conversation in a house between a user and {len(cast)} characters: "
                f"{names([c.name for c in cast])}. They walk from room to room; only those "
                "close enough hear what is said. The user is the host."
            ),
            "characters": {
                c.name: {"role": c.role, "about": c.tagline, "knows_best": c.topics} for c in cast
            },
            "conversation": [
                {
                    "speaker": self.label(line.speaker),
                    "said": line.text + (" (cut off)" if line.interrupted else ""),
                }
                for line in history[-HISTORY_LINES:]
            ],
        }
        if last_addressed:
            everyone = len(last_addressed) == len(cast)
            listed = names([self.label(c) for c in last_addressed])
            state["user_last_spoke_to"] = "everyone" if everyone else listed
        if quiet_for is not None:
            state["quiet_for_seconds"] = round(quiet_for)
            return state
        if self.where:
            state.update(self.where)
        said: dict[str, Any] = {"speaker": self.label(latest.speaker), "said": latest.text}
        if latest.speaker == USER:
            said["heard_via"] = "live speech-to-text, so a name may be misheard"
        if latest.heard_by is not None:
            heard = [self.label(c) for c in self.cast if c in latest.heard_by]
            said["heard_by"] = heard or "nobody"
            # The user may have walked away from whoever they spoke to last.
            away = [
                self.label(c) for c in last_addressed if c in self.cast and c not in latest.heard_by
            ]
            if latest.speaker == USER and away:
                said["out_of_earshot"] = (
                    f"{names(away)}, whom the user spoke to last, "
                    f"{'is' if len(away) == 1 else 'are'} too far away now to hear this"
                )
        state["latest"] = said
        return state


# What a character is told about the moment, appended to its view as a `[Note: …]` line. A group
# is told who was asked and who has already answered: "it's your turn now" keeps PhoneLLM from
# answering for the table or announcing that it will answer later.
NOTE_SWITCH = "The person wants your answer now, not {other}'s."
NOTE_GROUP_FIRST = (
    "The person said that to {who}. You answer first: reply to the person in one short sentence "
    "of your own."
)
NOTE_GROUP_NEXT = (
    "The person said that to {who}, and {done} {have} already replied. It's your turn now: reply "
    "to the person in one short sentence of your own."
)
NOTE_CHORUS = (
    "The person asked {who} that, and everyone is answering at once, out loud. Answer for "
    'yourself, in character, in as few words as you can: often just "Yes!", "Me!" or '
    '"Not me."'
)
NOTE_CHORUS_IF = (
    "The person asked only those it's true for to answer, and they're answering at once, you "
    "among them: answer out loud in as few words as you can, in character: often just "
    f'"Me!" (Only if it really isn\'t true for you, write {SILENT}.)'
)
NOTE_WELCOME = (
    "The person has just walked in, and everyone at the table says hi at once, out loud. Say hi "
    "to them in a few words, in character, the way only you would."
)
NOTE_NOT_HEARD = (
    "The person said that to {other}, who is too far away to hear it; you heard it. Answer the "
    "person yourself in one short sentence: you might tell them {other} can't hear them from here."
)
NOTE_ACT = {
    "dance": "As you answer, you start dancing.",
    "play": "As you answer, you head to the piano in the music room to play.",
    "music_on": "As you answer, you head to the gramophone in the hall to put a record on.",
    "music_off": "As you answer, you go and turn the music off.",
    "cook": "As you answer, you head to the stove in the kitchen to bake a cake.",
    "food": "As you answer, you go to get the person something to eat, to bring it to them.",
    "flower": (
        "As you answer, you go to pick the person a flower, to bring it to them. Don't say what "
        "color it'll be: you'll see when you've picked it."
    ),
    "water": "As you answer, you go to water the plants in the conservatory.",
    "stop": "As you answer, you stop what you're doing.",
    "hand": "As you answer, you hand the person what you're holding.",
}
# ...asked to dance with no music on: they ask for some first...
NOTE_NO_MUSIC = (
    "There's no dance music on (a record on the gramophone in the hall), so you don't dance "
    "yet: say, in a few words, that they'll need to put a record on first."
)
# ...a flower in the color asked for...
NOTE_FLOWER_COLOR = "As you answer, you go to pick {to} a {color} flower, to bring it to them."
# ...and when what they bring or hand over is for someone else.
NOTE_ACT_FOR = {
    "food": "As you answer, you go to get {to} something to eat, to bring it to them.",
    "flower": (
        "As you answer, you go to pick {to} a flower, to bring it to them. Don't say what color "
        "it'll be: you'll see when you've picked it."
    ),
    "hand": "As you answer, you hand {to} what you're holding.",
    "introduce": (
        "{to} isn't here, so as you answer, you lead the way to them: tell the person to follow "
        "you, in your own words ('sure, follow me!'). Don't introduce them yet: you will once "
        "you're all together."
    ),
}
# ...asked to introduce the user to someone who's right there.
NOTE_INTRODUCE_HERE = (
    "{to} is right here: as you answer, you turn to them, to introduce the person. Don't "
    "introduce them yet: you will in a moment."
)
# ...asked to introduce the user, without saying to whom.
NOTE_INTRODUCE_WHOM = "Ask the person who they'd like to meet."
# Something that happened in the house: what's said by whoever reacts to it...
NOTE_EVENT = {
    "gift": "The person has just given you {item}. React to it out loud, in a few words, in character.",
    "handed": (
        "You've just handed the person {item}. Say something to them as you do, in a few words."
    ),
    "handing": (
        "You're handing {to} {item}. Say so to {to} as you do, in a few words, like \"Here you "
        'go, {to}: {item}."'
    ),
    "received": (
        "{who} has just handed you {item}. Say something to {who} about it, in a few words, in "
        "character: thanks, say."
    ),
    "bell": (
        "Someone has just rung the dinner bell in the kitchen, and you're heading there. Say "
        "something about it out loud, in a few words, in character."
    ),
    "music_on": (
        "The person has just put a record on the gramophone in the hall, and music is playing. "
        "React to it, in a few words, in character."
    ),
    "music_off": "The person has just stopped the music. React to it, in a few words, in character.",
    "piano": (
        "The person has just plinked out a few notes on the piano. React to it, in a few words, "
        "in character."
    ),
    "picked": (
        "The person has just picked {item} from {place}, where it's yours to look after. React "
        "to it, in a few words, in character."
    ),
    "wish": (
        "The person has just tossed a coin into the fountain and made a wish. Say something "
        "about it, in a few words; you might ask them what they wished for."
    ),
    "baked": "The cake you were baking is ready, on the kitchen island. Say so out loud, in a few words.",
    "visit": (
        "You've just dropped in on {to} in {place} for a chat. Say something to {to} by name: a "
        "bit of news, a question, or a tease, in one or two short sentences."
    ),
    "introduce": (
        "You've brought the person to meet {to}, and you're all together now. Introduce them to "
        "each other, by name, and nothing more: say to {to} who the person is (by name, if "
        "they've told you it), and to the person who {to} is, in a few words each, like \"Juno, "
        "meet Sam. Sam, this is Juno, our musician.\" Don't talk about yourself."
    ),
}
# ...and what everyone who saw or heard it knows, in the transcript.
NOTE_HAPPENED = {
    "gift": "The person gave {to} {item}.",
    "handed": "{who} handed {to} {item}.",
    "bell": "Someone rang the dinner bell in the kitchen.",
    "music_on": "The person put a record on the gramophone in the hall.",
    "music_off": "The person stopped the music.",
    "piano": "The person plinked a few notes on the piano.",
    "picked": "The person picked {item} from {place}.",
    "wish": "The person tossed a coin into the fountain and made a wish.",
    "baked": "{who} baked a cake; it's on the kitchen island.",
    "visit": "{who} dropped in on {to} in {place} for a chat.",
    "introduce": "{who} introduced the person to {to}.",
}
NOTE_MOVE = {
    COME: "As you answer, you walk over to the person.",
    FOLLOW: "As you answer, you start following the person, wherever they go.",
    WAIT: "As you answer, you stop and stay where you are.",
    HOME: "As you answer, you head back to {home}.",
    GO: "As you answer, you head to {area}.",
}
NOTE_REPLY = (
    "{other} just spoke to you or about you. It's your turn: answer {other} in one or two short "
    "sentences."
)
NOTE_WRAP = (
    "{other} just spoke to you or about you. This chat is winding down: answer {other} in one "
    "short sentence that lands it (agree, finish the joke, or, if the person's with you, turn it "
    "back to them), and don't ask anyone anything new."
)
NOTE_REACT = (
    "{other} just said that. React out loud on the spot, in one to four words, the way you would "
    "at the table: a laugh, a groan, 'oh no', 'never!'. Nothing more."
)
NOTE_LULL = (
    "It has gone quiet for a moment. Break the silence: say something to the person, or to "
    "someone with you by name, picking up something said earlier or something on your mind, in "
    "one or two short sentences."
)
NOTE_CARRY_ON_CUT = (
    "The person cut in, then told you all to carry on. Pick up where you left off, in one or two "
    "short sentences."
)
NOTE_CARRY_ON_TABLE = (
    "The person wants the table to carry on talking. Keep the conversation going: say your bit to "
    "someone at the table, in one or two short sentences."
)
NOTE_CARRY_ON = "Carry on."
NOTE_JOINED = "The person joins the room."


# --- Jev's questions --------------------------------------------------------------------------


def addressee_question(cast: Sequence[Character]) -> ChoiceQuestion:
    """Who the user is talking to in `latest`: one character, or a group."""
    return ChoiceQuestion(
        instructions={
            "question": (
                "The user just said `latest`. Who are they talking to, and so who should answer?"
            ),
            "clues": [
                "A name the user says tells you who, even when speech-to-text misspells it.",
                "A question about something one of them said earlier ('so who liked blue?', "
                "'which of you had the boat?') goes to whoever said it: look for it in "
                "`conversation`.",
                "A reply to what a character just said or asked goes to that character.",
                "Asking for something someone has ('can I have that flower back?', 'pass me the "
                "cake'), with no name, is for whoever is holding it (`holding`).",
                "A follow-up that carries on the user's previous question, such as 'and number?', "
                "'why?' or 'really?', goes to whoever they spoke to last (`user_last_spoke_to`), "
                "unless it names someone else, or they've walked away from them "
                "(`latest.out_of_earshot`).",
                "The user walks around the house. Once they've walked away from whoever they "
                "spoke to last (`latest.out_of_earshot`), anything without that person's name is "
                "for those with them now (`latest.heard_by`, `with_the_user`), not for whoever "
                "they spoke to last.",
                "A correction such as 'not you' or 'I meant the other one' goes to someone other "
                "than the character who just spoke.",
                "With no name and nothing to reply to, the subject decides: whoever knows it best "
                "in `characters`.",
                "Meeting someone new: a greeting or a question for someone they don't know yet "
                "('oh, hello, who are you?', 'and you are?', 'nice to meet you', 'what's your "
                "name?'), with someone they've already talked with (`user_has_talked_with`) "
                "among those who heard it, is for whoever they haven't met (`user_has_not_met`): "
                "one of them, or all of them if there are several. They already know the ones "
                "they've talked with.",
                "Otherwise, a greeting, a goodbye, thanks or a question with no name for the "
                "whole table ('hello!', 'who wants cake?') is for everyone.",
                "`latest.heard_by` is who was close enough to hear it. With no name, it's for "
                "them: one of them, or all of them. A name still says who, even someone who "
                "didn't hear it.",
            ],
        },
        options={
            **{c.id: {"who": c.name} for c in cast},
            GROUP: {
                "who": (
                    "several of them at once: everyone, both of them, or two or more of them "
                    "named together"
                ),
                "examples": [
                    "Hello!",
                    "You two, follow me.",
                    "Both of you, come with me.",
                    "Who's here?",
                    "Who wants cake?",
                    "What do you all think?",
                    "Can you each introduce yourselves?",
                    f"{cast[0].name} and {cast[1].name}, what do you reckon?",
                    "Thanks, everyone.",
                ],
            },
        },
    )


def included_question(character: Character) -> YesNoQuestion:
    """Whether `character` is one of those the user wants an answer from (used for a group)."""
    name = character.name
    return YesNoQuestion(
        instructions=(
            f"In `latest`, does the user want an answer from {name}, alone or together with others?"
        ),
        yes=(
            f"{name} is named, or included in 'everyone', 'anyone' or 'you all', or the user "
            f"greets, thanks, asks or says goodbye to the whole table, or asks about something "
            f"{name} said, or carries on their previous question and {name} was among those they "
            "spoke to last (`user_last_spoke_to`)"
        ),
        no=(
            f"the user is talking to someone else, or to a few others that leave {name} out, or "
            f"they're meeting someone new ('who are you?', 'nice to meet you') and have already "
            f"talked with {name} (`user_has_talked_with`)"
        ),
    )


def chorus_question() -> YesNoQuestion:
    """For a group: can they all answer at once, or should they answer in turn?"""
    return YesNoQuestion(
        instructions=(
            "If the user is speaking to several people at the table in `latest`, can each of "
            "them answer straight away, all at the same time, in a few words of their own?"
        ),
        yes=(
            "a quick show-of-hands question or a greeting to everyone: who wants cake, has "
            "anyone been to Paris, is everyone ready, what's everyone's favourite fruit, hello "
            "everyone, goodnight all"
        ),
        no=(
            "each needs a considered answer, or the answers build on each other: introduce "
            "yourselves, tell me a story each, what do you all think about this, discuss; or the "
            "user is only talking to one person"
        ),
    )


def only_if_question() -> YesNoQuestion:
    """For a chorus: does the user only want to hear from those something is true of?"""
    return YesNoQuestion(
        instructions=(
            "In `latest`, does the user only want to hear from those at the table that something "
            "is true of, so that the rest should stay quiet?"
        ),
        yes=(
            "they ask for a yes, a hand or a word only from those it applies to, so the others "
            "say nothing: 'say yes if you want some', 'hands up if you've been on a boat', "
            "'shout if you're hungry', 'anyone who can sing, say so'"
        ),
        no=(
            "an open question everyone answers either way, yes or no: 'who wants cake?', 'has "
            "anyone been to Paris?', 'what's everyone's favourite fruit?'; or it isn't a question "
            "for the whole table"
        ),
    )


def true_of_question(character: Character) -> YesNoQuestion:
    """For a "hands up if…": is it true of `character`? (Only used when `only_if` says so.)"""
    name = character.name
    return YesNoQuestion(
        instructions=(
            f"Suppose `latest` asks only those something is true of to answer ('hands up if…', "
            f"'say yes if…'). Is it true of {name}, from who they are (`characters`) and what "
            "they've said in `conversation`?"
        ),
        yes=f"it's true of {name}, or likely so for someone like them",
        no=f"it isn't true of {name}, or unlikely for someone like them",
    )


def intent_question() -> ChoiceQuestion:
    """What the user wants, as the table's host."""
    return ChoiceQuestion(
        instructions={
            "question": (
                "The user, who is hosting the table, just said `latest`, often cutting in while "
                "the characters were talking. What do they want?"
            ),
            "clues": [
                "Almost everything is `answer`: a question, a remark, a greeting, a new topic, or "
                "turning the conversation to someone.",
                "Only an explicit invitation to keep going is `carry_on`.",
                "Only an explicit request for quiet, with nothing else asked, is `hush`.",
                "Cutting in to stop the table and then asking or telling it something ('woah, "
                "woah, just say yes if you want cake', 'hang on, Otto, what do you think?', "
                "'stop, stop, who said that?') is `answer`: they stopped the table to be "
                "answered.",
            ],
        },
        options={
            ANSWER: {
                "what": (
                    "an answer or a reaction from someone: they ask, tell or greet someone, change "
                    "the subject, or turn the conversation to someone ('Otto, what about you?')"
                )
            },
            CARRY_ON: {
                "what": (
                    "for the table to carry on what it was talking about: 'go on', 'keep going', "
                    "'carry on', 'don't mind me', 'sorry, as you were'"
                )
            },
            HUSH: {
                "what": (
                    "quiet: the table should stop talking for now: 'quiet please', 'settle down', "
                    "'hold on, let me think', 'stop', 'shh', 'one at a time'"
                )
            },
        },
    )


def reply_question(speaker: Character, cast: Sequence[Character]) -> ChoiceQuestion:
    """After `speaker`'s line: does someone else at the table answer it, or is it the user's go?

    Every option is described, not just named: with bare names beside a described "the user",
    Jev gave a line pointing the user to Maya ("that's Maya's colour") back to the user.
    """
    s = speaker.name
    options: dict[str, Any] = {
        c.id: {
            "what": (
                f"{c.name}: {s} speaks to or about {c.name}, or points the user to {c.name}: asks "
                f"them something, teases them, says something about them, or says the user's "
                f"question is really theirs ('that's {c.name}'s', 'ask {c.name}')"
            )
        }
        for c in cast
        if c.id != speaker.id
    }
    options[USER] = {
        "what": (
            f"the user: {s} answers the user without pointing them to anyone else, asks the user "
            "something, or just agrees, thanks or wraps up"
        )
    }
    return ChoiceQuestion(
        instructions={
            "question": (
                f"{s} just said `latest` out loud, with the user and everyone at the table "
                "listening. Who speaks next: someone else at the table, or the user?"
            ),
            "clues": [
                "Pointing the user to someone ('that's Maya's colour', 'ask Theo', 'Edith knows') "
                "hands that person the floor, even while answering the user.",
                "Someone teased, asked or talked about gets to answer when there's something for "
                "them to say.",
                "A passing mention in a line that isn't about them, with nothing for them to add, "
                "needs no answer.",
            ],
        },
        options=options,
    )


def momentum_question(speaker: Character) -> ScoreQuestion:
    """After `speaker`'s line: how much it asks to be answered by someone at the table."""
    return ScoreQuestion(
        instructions=(
            f"{speaker.name} just said `latest` at the table. How much does it give the others "
            "to answer: how much life does it put into the conversation?"
        ),
        levels=[
            "winding down: it agrees, thanks, wraps up, or repeats a joke, jab or point already "
            "made in `conversation`; there's nothing new to answer",
            "ticking over: a mild remark that someone could answer, or could let pass",
            "lively: a new joke, jab, claim or story someone at the table will want to answer",
            "begs an answer: a direct question to, or a provocation of, someone at the table",
        ],
    )


def react_question(speaker: Character, cast: Sequence[Character]) -> ChoiceQuestion:
    """After `speaker`'s line: does someone react out loud as it ends (a laugh, a groan)?"""
    s = speaker.name
    options: dict[str, Any] = {
        c.id: {"what": f"{c.name}: it's funny, cheeky or surprising to {c.name}, or about them"}
        for c in cast
        if c.id != speaker.id
    }
    options[NOBODY] = {
        "what": "nobody: an ordinary remark or question with nothing to react to on the spot"
    }
    return ChoiceQuestion(
        instructions={
            "question": (
                f"As {s} finishes saying `latest`, does someone else at the table react out loud "
                "on the spot, with a laugh, a groan or a quick word?"
            ),
            "clues": [
                "Only a punchline, a tease, a surprise or something outrageous gets a reaction.",
                "Most lines get none.",
            ],
        },
        options=options,
    )


def lull_question(cast: Sequence[Character]) -> ChoiceQuestion:
    """The table has gone quiet: who would naturally break the silence?"""
    return ChoiceQuestion(
        instructions={
            "question": (
                "The table has gone quiet, and the user hasn't said anything for "
                "`quiet_for_seconds` seconds. Who would naturally break the silence now?"
            ),
            "clues": [
                "Someone who was teased, asked or talked about in `conversation` and never got to "
                "answer.",
                "Someone with an unfinished story or point.",
                "Someone whose interests fit what was being talked about.",
                "Not whoever spoke last, unless they were cut off.",
            ],
        },
        options={c.id: {"who": c.name, "knows_best": c.topics} for c in cast},
    )


# What else people call the house's rooms, so "the living room" is the hall.
AREA_ALIASES = {
    "hall": "the living room, the lounge, the main room, the big room with the columns",
    "conservatory": "the garden, the greenhouse, the plant room, the room with the fountain",
    "kitchen": "the dining room, where the food is",
    "music": "the music room, the stage, where the piano is",
    "gallery": "the art room, the room with the sculptures and paintings",
}


def move_question(areas: dict[str, str]) -> ChoiceQuestion:
    """Whether the user asks whoever they're talking to to move: come over, follow them, wait,
    go home, or go to one of the house's areas (by id, in `areas`)."""
    options: dict[str, Any] = {
        STAY: {
            "what": (
                "nothing about moving now: a question, a remark, a greeting, or talk about "
                "places or walking that doesn't ask anyone to move"
            )
        },
        COME: {
            "what": "come over to where the user is, now",
            "examples": ["Come here", "Come over here", "Over here!", "Join us", "Gather round"],
        },
        FOLLOW: {
            "what": "follow the user, wherever they go",
            "examples": ["Follow me", "Come with me", "Let's go", "Walk with me", "This way"],
        },
        WAIT: {
            "what": "stay where they are, or stop following the user",
            "examples": ["Wait here", "Stay there", "Stop following me", "Don't move"],
        },
        HOME: {
            "what": "go back to their own place in the house (`homes`)",
            "examples": ["Go back", "You can go now", "Back to your plants", "Off you go"],
        },
    }
    for area, name in areas.items():
        options[f"{GO}{area}"] = {
            "what": f"go to {name}"
            + (f" (also called {AREA_ALIASES[area]})" if area in AREA_ALIASES else ""),
            "examples": [f"Go to {name}", f"Meet me in {name}", f"Wait for me in {name}"],
        }
    return ChoiceQuestion(
        instructions={
            "question": (
                "In `latest`, is the user asking whoever they're talking to to move somewhere "
                "now, and if so, where?"
            ),
            "clues": [
                "Most of what the user says asks nobody to move: `stay`.",
                "Coming over to the user is `come`; going along with them as they go is `follow`.",
                "Sending someone back to their own room (`homes`) is `home`.",
                "Asking someone to fetch, bring or pick something, or to introduce them to "
                "someone, isn't a move (`stay`): the errand takes them where they need to go.",
            ],
        },
        options=options,
    )


def act_question() -> ChoiceQuestion:
    """Whether the user asks whoever they're talking to to do something, other than move."""
    return ChoiceQuestion(
        instructions={
            "question": (
                "In `latest`, is the user asking whoever they're talking to to do something now "
                "(other than moving somewhere), and if so, what?"
            ),
            "clues": [
                "Most of what the user says asks nothing: `none`.",
                "Asking where someone is, or what they're doing, is `none`.",
                "Asking for something to eat is `food`; asking someone to make it is `cook`.",
            ],
        },
        options=dict(ACTS),
    )


def for_question(cast: Sequence[Character]) -> ChoiceQuestion:
    """Who something the user asks to be brought or handed over is for: them, or someone else."""
    options: dict[str, Any] = {
        USER: {
            "who": (
                "the user themselves, or nothing's being brought, handed over or introduced: 'can "
                "I have…', 'bring me…', 'give me…', 'I'm hungry'"
            )
        },
        **{
            c.id: {
                "who": (
                    f"{c.name}: 'give {c.name} a flower', 'bring {c.name} some cake', 'hand that "
                    f"to {c.name}', 'get {c.name} something to eat', 'introduce me to {c.name}'"
                )
            }
            for c in cast
        },
    }
    return ChoiceQuestion(
        instructions={
            "question": (
                "In `latest`, if the user asks for something to be brought, given or handed over "
                "(something to eat, a flower, or what someone is holding), who is it for? Or, if "
                "they ask to be introduced to someone, who do they want to meet?"
            ),
            "clues": [
                "It's for the user unless they name someone else to give it to.",
                "Whoever is asked to do it isn't who it's for: 'Theo, give Maya some cake' is for "
                "Maya.",
                "Asked to be introduced ('introduce me to Juno', 'can I meet Theo?'), it's whoever "
                "they want to meet (Juno, Theo), never the user.",
            ],
        },
        options=options,
    )


def doing_question(speaker: Character) -> ChoiceQuestion:
    """Whether `speaker` sets about doing something as they say their line."""
    return ChoiceQuestion(
        instructions={
            "question": (
                f"As {speaker.name} says `latest`, are they setting about doing something "
                "themselves, right now, and if so, what?"
            ),
            "clues": [
                "Most lines do nothing: `none`.",
                "Only a line that says they're doing it now counts: offering, suggesting or "
                "talking about it doesn't.",
            ],
        },
        options=dict(DOINGS),
    )


def feel_question() -> ChoiceQuestion:
    """How those who heard the user's line take it, which their posture shows as they listen."""
    return ChoiceQuestion(
        instructions=(
            "The characters who heard the user say `latest` (`latest.heard_by`) are listening. "
            "How do they take it? Their posture shows it."
        ),
        options=dict(MOODS),
    )


def mood_question(speaker: Character) -> ChoiceQuestion:
    """The mood `speaker` says their line in, which their posture shows as they speak."""
    return ChoiceQuestion(
        instructions=(
            f"{speaker.name} is saying `latest` out loud. In what mood? Their posture shows it "
            "as they speak."
        ),
        options=dict(MOODS),
    )


def gesture_question(speaker: Character) -> ChoiceQuestion:
    """What `speaker` does with their hands or head as they say their line, if anything."""
    return ChoiceQuestion(
        instructions={
            "question": (
                f"As {speaker.name} says `latest`, what do they do with their hands or head, "
                "if anything?"
            ),
            "clues": ["Most lines get no particular gesture."],
        },
        options=dict(GESTURES),
    )


def taken_question(speaker: Character) -> ChoiceQuestion:
    """How the others who hear `speaker`'s line take it."""
    return ChoiceQuestion(
        instructions=(
            f"The others close enough to hear {speaker.name} say `latest` are listening. How do "
            "they take it? Their posture shows it."
        ),
        options=dict(MOODS),
    )


def top(values: dict[str, float], floor: float) -> str | None:
    """The likeliest of `values`, if Jev is at least `floor` sure of it."""
    if not values:
        return None
    best = max(values, key=lambda k: values[k])
    return best if values[best] >= floor else None


# --- Asking Jev -------------------------------------------------------------------------------


@dataclass
class Reading:
    """One answer from Jev, as the debug panel shows it."""

    kind: str  # "preview" (words still being spoken), "route" (a user turn), "reply" (a line)
    # or "lull" (the table has gone quiet)
    speaker: str  # whose words were read: USER, or a character for a reply
    heard: str
    choice: str | None
    probabilities: dict[str, float] = field(default_factory=dict)  # the choice: each id, GROUP
    included: dict[str, float] = field(default_factory=dict)  # each id: one of those asked?
    chorus: float | None = None  # a route: can a group answer at once?
    only_if: float | None = None  # ...and only those it's true for?
    true_of: dict[str, float] = field(default_factory=dict)  # ...each id: is it true of them?
    intent: dict[str, float] = field(default_factory=dict)  # a route: ANSWER, CARRY_ON, HUSH
    momentum: float | None = None  # a reply read: how much the line asks to be answered, 0 to 1
    react: dict[str, float] = field(default_factory=dict)  # a reply read: each id, NOBODY
    move: dict[str, float] = field(default_factory=dict)  # a route: STAY, COME, … or GO + area
    act: dict[str, float] = field(default_factory=dict)  # a route: what they're asked to do
    for_: dict[str, float] = field(default_factory=dict)  # ...and who what they bring is for
    feel: dict[str, float] = field(default_factory=dict)  # a route: how the listeners take it
    mood: dict[str, float] = field(default_factory=dict)  # a reply read: the speaker's mood
    gesture: dict[str, float] = field(default_factory=dict)  # ...their gesture
    taken: dict[str, float] = field(default_factory=dict)  # ...how the others take it
    doing: dict[str, float] = field(default_factory=dict)  # ...what the speaker sets about
    ms: float = 0.0  # how long the caller waited: 0 when the answer was already cached
    cached: bool = False
    state: dict[str, Any] = field(default_factory=dict)
    error: str | None = None
    # With a recency prior applied (`weigh`): Jev's own probabilities, and who was favoured.
    raw: dict[str, float] | None = None
    favoured: str | None = None
    weight: float = 1.0

    def p(self, option: str) -> float:
        return self.probabilities.get(option, 0.0)

    def addressed(self, character: str) -> float:
        """How likely `character` is being talked to: alone, or as one of a group."""
        return self.p(character) + self.p(GROUP) * self.included.get(character, 0.0)

    def group(self, order: Sequence[str]) -> list[str]:
        """Who answers a group turn: everyone Jev is sure is included, most surely asked first,
        or, when fewer than two are, everyone at the table in `order`."""
        members = [c for c in order if self.included.get(c, 0.0) >= INCLUDED_FLOOR]
        if len(members) < 2:
            return list(order)
        return sorted(members, key=lambda c: -self.included.get(c, 0.0))

    def wants(self, floor: float) -> str:
        """What the user wants as host: CARRY_ON or HUSH when Jev is at least `floor` sure,
        otherwise ANSWER."""
        for option in (HUSH, CARRY_ON):
            if self.intent.get(option, 0.0) >= floor:
                return option
        return ANSWER

    def wants_move(self, floor: float) -> tuple[str, str | None] | None:
        """The move the user asks for, if Jev is at least `floor` sure: an action (COME,
        FOLLOW, WAIT, HOME or "go") and, for "go", the area."""
        best = top(self.move, floor)
        if best is None or best == STAY:
            return None
        if best.startswith(GO):
            return "go", best.removeprefix(GO)
        return best, None

    def wants_act(self, floor: float) -> str | None:
        """What the user asks whoever they're talking to to do, if Jev is at least `floor` sure."""
        best = top(self.act, floor)
        return None if best in (None, NO_ACT) else best

    def gives_to(self, floor: float) -> str | None:
        """Who something the user asks to be brought or handed over is for, if it's a character
        and Jev is at least `floor` sure: otherwise, it's for the user."""
        best = top(self.for_, floor)
        return None if best in (None, USER) else best

    def does(self, floor: float) -> str | None:
        """What the speaker of a line sets about doing, if Jev is at least `floor` sure."""
        best = top(self.doing, floor)
        return None if best in (None, NO_ACT) else best

    def reactor(self, floor: float) -> str | None:
        """Who reacts out loud, if Jev is at least `floor` sure someone does."""
        if not self.react:
            return None
        who = max(self.react, key=lambda k: self.react[k])
        return who if who != NOBODY and self.react[who] >= floor else None

    def to_message(self) -> dict[str, Any]:
        def rounded(values: dict[str, float]) -> dict[str, float]:
            return {k: round(v, 4) for k, v in values.items()}

        members: list[str] = []
        if self.choice == GROUP:
            members = self.group(list(self.included))
        elif self.choice:
            members = [self.choice]
        return {
            "type": "jev",
            "kind": self.kind,
            "speaker": self.speaker,
            "heard": self.heard,
            "choice": self.choice,
            "probabilities": rounded(self.probabilities),
            "included": rounded(self.included),
            "addressed": {
                c: round(self.addressed(c), 4)
                for c in (self.included or [k for k in self.probabilities if k != USER])
            },
            "members": members,  # who answers: the one chosen, or the group in order
            "chorus": None if self.chorus is None else round(self.chorus, 4),
            "only_if": None if self.only_if is None else round(self.only_if, 4),
            "true_of": rounded(self.true_of),
            "intent": rounded(self.intent),
            "momentum": None if self.momentum is None else round(self.momentum, 4),
            "react": rounded(self.react),
            "move": rounded(self.move),
            "act": rounded(self.act),
            "for": rounded(self.for_),
            "doing": rounded(self.doing),
            "feel": rounded(self.feel),
            "mood": rounded(self.mood),
            "gesture": rounded(self.gesture),
            "taken": rounded(self.taken),
            "ms": round(self.ms),
            "cached": self.cached,
            "state": self.state,
            "error": self.error,
            "raw": rounded(self.raw) if self.raw else None,
            "favoured": self.favoured,
            "weight": self.weight,
            "at": time.time(),
        }


class Referee:
    """Asks Jev about the room: each set of questions in one request.

    Answers are cached by question and state, and a request already in flight is shared, so a read
    made early (while the user is still speaking, or while a line is still playing) answers the
    final one when nothing has changed.
    """

    def __init__(self, classifier: BaseClassifier, cast: Sequence[Character]) -> None:
        self._classifier = classifier
        self._cast = list(cast)
        self._questions: dict[str, dict[str, ClassifierQuestion]] = {
            ADDRESSEE: self._addressee_questions({}),
            **{
                f"{REPLY}{c.id}": {
                    NEXT: reply_question(c, cast),
                    MOMENTUM: momentum_question(c),
                    REACT: react_question(c, cast),
                    MOOD: mood_question(c),
                    GESTURE: gesture_question(c),
                    TAKEN: taken_question(c),
                    DOING: doing_question(c),
                }
                for c in cast
            },
            LULL: {LULL: lull_question(cast)},
        }
        self._cache: OrderedDict[str, asyncio.Task[dict[str, ClassifierResult]]] = OrderedDict()
        self.asked = 0
        self.cached = 0

    def _addressee_questions(self, areas: dict[str, str]) -> dict[str, ClassifierQuestion]:
        cast = self._cast
        return {
            ADDRESSEE: addressee_question(cast),
            **{f"{INCLUDED}{c.id}": included_question(c) for c in cast},
            CHORUS: chorus_question(),
            ONLY_IF: only_if_question(),
            **{f"{TRUE_OF}{c.id}": true_of_question(c) for c in cast},
            INTENT: intent_question(),
            MOVE: move_question(areas),
            ACT: act_question(),
            FOR: for_question(cast),
            FEEL: feel_question(),
        }

    def set_areas(self, areas: dict[str, str]) -> None:
        """The house's areas (id → name), which the user can send characters to."""
        self._questions[ADDRESSEE] = self._addressee_questions(areas)
        for key in [k for k in self._cache if k.startswith(f'["{ADDRESSEE}"')]:
            del self._cache[key]

    @property
    def model(self) -> str | None:
        return self._classifier.model

    async def addressee(
        self,
        transcript: Transcript,
        history: Sequence[Line],
        heard: str,
        *,
        last_addressed: Sequence[str] = (),
        kind: str = "route",
        heard_by: frozenset[str] | None = None,
    ) -> Reading:
        latest = Line(USER, normalize(heard), heard_by=heard_by)
        state = transcript.for_jev(history, latest, last_addressed)
        reading = Reading(kind, USER, normalize(heard), None, state=state)
        results = await self._ask(reading, ADDRESSEE)
        if results is not None:
            choice, chorus, intent = results[ADDRESSEE], results[CHORUS], results[INTENT]
            only_if = results[ONLY_IF]
            assert isinstance(choice, ChoiceResult) and isinstance(chorus, YesNoResult)
            assert isinstance(intent, ChoiceResult) and isinstance(only_if, YesNoResult)
            reading.only_if = only_if.probability
            reading.choice = choice.choice
            reading.probabilities = dict(choice.probabilities)
            reading.chorus = chorus.probability
            reading.intent = dict(intent.probabilities)
            for name, target in (
                (MOVE, reading.move),
                (ACT, reading.act),
                (FOR, reading.for_),
                (FEEL, reading.feel),
            ):
                result = results.get(name)
                if isinstance(result, ChoiceResult):
                    target.update(result.probabilities)
            for name, result in results.items():
                if name.startswith(INCLUDED) and isinstance(result, YesNoResult):
                    reading.included[name.removeprefix(INCLUDED)] = result.probability
                elif name.startswith(TRUE_OF) and isinstance(result, YesNoResult):
                    reading.true_of[name.removeprefix(TRUE_OF)] = result.probability
        return reading

    async def reply(
        self,
        transcript: Transcript,
        history: Sequence[Line],
        speaker: str,
        line: str,
        heard_by: frozenset[str] | None = None,
    ) -> Reading:
        """After `speaker`'s `line`: who, if anyone, answers it (`USER`: nobody, the user's go),
        how much momentum it has, and who reacts to it out loud."""
        state = transcript.for_jev(history, Line(speaker, normalize(line), heard_by=heard_by))
        reading = Reading("reply", speaker, normalize(line), None, state=state)
        results = await self._ask(reading, f"{REPLY}{speaker}")
        if results is not None:
            choice, momentum, react = results[NEXT], results[MOMENTUM], results[REACT]
            assert isinstance(choice, ChoiceResult) and isinstance(momentum, ScoreResult)
            assert isinstance(react, ChoiceResult)
            reading.choice = choice.choice
            reading.probabilities = dict(choice.probabilities)
            reading.momentum = momentum.score / (len(momentum.levels) - 1)
            reading.react = dict(react.probabilities)
            for name, target in (
                (MOOD, reading.mood),
                (GESTURE, reading.gesture),
                (TAKEN, reading.taken),
                (DOING, reading.doing),
            ):
                result = results.get(name)
                if isinstance(result, ChoiceResult):
                    target.update(result.probabilities)
        return reading

    async def lull(self, transcript: Transcript, quiet_for: float) -> Reading:
        """The table has been quiet for `quiet_for` seconds: who breaks the silence?"""
        history = list(transcript.lines)
        state = transcript.for_jev(history, Line(USER, ""), quiet_for=quiet_for)
        reading = Reading("lull", USER, "", None, state=state)
        results = await self._ask(reading, LULL)
        if results is not None:
            choice = results[LULL]
            assert isinstance(choice, ChoiceResult)
            reading.choice = choice.choice
            reading.probabilities = dict(choice.probabilities)
        return reading

    async def _ask(self, reading: Reading, questions: str) -> dict[str, ClassifierResult] | None:
        """Ask one set of questions about `reading.state`, sharing the cache; fills in the
        reading's timing, and its error if Jev couldn't answer."""
        started = time.perf_counter()
        key = json.dumps([questions, reading.state], sort_keys=True)
        task = self._cache.get(key)
        reading.cached = task is not None and task.done()
        if task is None:
            task = asyncio.create_task(
                self._classifier.ask(reading.state, self._questions[questions])
            )
            task.add_done_callback(lambda t, key=key: self._forget_failure(key, t))
            self._cache[key] = task
            while len(self._cache) > JEV_CACHE_SIZE:
                self._cache.popitem(last=False)
            self.asked += 1
        else:
            self._cache.move_to_end(key)
            self.cached += 1
        try:
            # Shielded: a caller that gives up leaves the answer to the cache.
            return await asyncio.shield(task)
        except asyncio.CancelledError:
            raise
        except Exception as error:  # noqa: BLE001 — a failed read is shown, never fatal
            reading.error = f"{type(error).__name__}: {error}"
            return None
        finally:
            reading.ms = 0.0 if reading.cached else (time.perf_counter() - started) * 1000

    def _forget_failure(self, key: str, task: asyncio.Task) -> None:
        if task.cancelled() or task.exception() is not None:
            if self._cache.get(key) is task:
                del self._cache[key]
            if not task.cancelled():
                logger.warning(f"Jev: {task.exception()}")

    async def close(self) -> None:
        for task in self._cache.values():
            if not task.done():
                task.cancel()
        self._cache.clear()


# The colors of the conservatory's flowers (RoomTypes.cpp), which someone can ask for.
FLOWER_COLORS = ("red", "yellow", "pink", "white", "purple", "orange")


def flower_color(text: str) -> str | None:
    """The color of flower `text` asks for or promises, if it names one of the conservatory's
    ("a yellow one, please"), and only one."""
    lowered = text.lower()
    said = {
        c
        for c in FLOWER_COLORS
        if re.search(rf"\b{c}\b", lowered) and not re.search(rf"\b(not|no)\s+{c}\b", lowered)
    }
    return said.pop() if len(said) == 1 else None


# Words speech-to-text writes that look like a name but aren't one ("the" for Theo).
NOT_NAMES = {"the", "then", "they", "them", "there", "may", "you", "your", "yeah"}


def mentions(text: str, name: str) -> bool:
    """Whether `text` says `name`, or something speech-to-text might have made of it ("Junot",
    "Mya")."""
    target = name.lower()
    for word in re.findall(r"[a-z]+", text.lower()):
        if word == target:
            return True
        if (
            word not in NOT_NAMES
            and abs(len(word) - len(target)) <= 1
            and SequenceMatcher(None, word, target).ratio() >= 0.8
        ):
            return True
    return False


def weigh(reading: Reading, favoured: str | None, weight: float) -> Reading:
    """A recency prior on Jev's choice: `favoured` (whoever the user spoke to last, a character
    or GROUP) counts `weight` times as likely before the words are weighed, then the choice is
    renormalised. Jev's probabilities are calibrated, so a light weight only settles near-ties:
    a name, a question about what someone said, or a correction outweighs it."""
    probabilities = reading.probabilities
    if not favoured or weight == 1 or favoured not in probabilities or reading.error:
        return reading
    scaled = {k: p * (weight if k == favoured else 1.0) for k, p in probabilities.items()}
    total = sum(scaled.values()) or 1.0
    weighted = {k: p / total for k, p in scaled.items()}
    return replace(
        reading,
        probabilities=weighted,
        choice=max(weighted, key=lambda k: weighted[k]),
        raw=dict(probabilities),
        favoured=favoured,
        weight=weight,
    )
