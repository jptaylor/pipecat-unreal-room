"""Who can hear whom in the house, as the game says.

The house has rooms with doorways between them. The player's voice carries as far as they speak
up (a ring around them in the game shows how far), half as far through a wall, and only the
characters inside it hear what they say. A character's voice carries a set distance. The game
works all that out and tells the bot, over RTVI client messages:

- `world`: the house's areas, e.g. {"areas": [{"id": "kitchen", "name": "the kitchen"}]}.
- `earshot`: who heard what the player is saying, since they started: {"heard": ["maya"]}.
- `space`, whenever it changes: where everyone is, who can hear each character, and what each
  is doing: {"user": {"area": "hall"}, "characters": {"maya": {"area": "conservatory",
  "hears": ["theo", "user"], "intent": "home"}}}.
- `met`: the player has come up to a character for the first time: {"who": "maya"}.

Before the game has said anything (the text harness, the tests), everyone hears everything, as
at the kitchen table.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any

from config import Character

USER = "user"


# What the person's looking at is in the notes only for what they mean by "this" or "those".
NOTE_LOOKING_ONLY = (
    'That\'s only so you know what they mean by "this", "that" or "those": don\'t bring it up '
    "unless they do."
)


@dataclass
class Space:
    cast: Sequence[Character]
    areas: dict[str, str] = field(default_factory=dict)  # id → name, e.g. "the kitchen"
    known: bool = False  # whether the game has said where everyone is
    user_area: str = ""
    where: dict[str, str] = field(default_factory=dict)  # character → area id
    # Who hears each character: other characters, and USER if the player does.
    hears: dict[str, set[str]] = field(default_factory=dict)
    intent: dict[str, str] = field(default_factory=dict)  # character → "home", "follow", …
    doing: dict[str, str] = field(default_factory=dict)  # character → e.g. "playing the piano"
    holding: dict[str, str] = field(default_factory=dict)  # character → e.g. "a flower"
    music: str = ""  # the area where music's playing, if any
    cake: bool = False  # whether there's a cake on the kitchen island
    user_holding: str = ""  # what the player's holding
    # What the player's looking at (a character's id, or a thing, e.g. "the flowers"), and what
    # they looked at just before, as the game's cone of their view finds them.
    user_looking_at: str = ""
    user_looked_at: str = ""
    user_also_seeing: str = ""  # looking at someone, the thing in view with them
    heard: set[str] = field(default_factory=set)  # who heard what the player is saying
    earshot_known: bool = False  # whether the game has said who heard the player yet
    talked: set[str] = field(default_factory=set)  # the characters the user has talked with
    # Those someone's bringing the user to meet, who'll introduce them (the director's): they
    # don't ask the user who they are (NOTE_STRANGER).
    being_introduced: set[str] = field(default_factory=set)

    @property
    def ids(self) -> list[str]:
        return [c.id for c in self.cast]

    def name(self, character: str) -> str:
        return next((c.name for c in self.cast if c.id == character), character)

    def area_name(self, area: str) -> str:
        return self.areas.get(area, area.replace("_", " ") if area else "somewhere")

    # --- From the game ---------------------------------------------------------------------------

    def world(self, data: dict[str, Any]) -> None:
        for area in data.get("areas") or []:
            if isinstance(area, dict) and area.get("id"):
                self.areas[str(area["id"])] = str(area.get("name") or area["id"])

    def update(self, data: dict[str, Any]) -> None:
        self.known = True
        user = data.get("user") or {}
        self.user_area = str(user.get("area") or "")
        self.user_looking_at = str(user.get("looking_at") or "")
        self.user_looked_at = str(user.get("looked_at") or "")
        self.user_also_seeing = str(user.get("also_seeing") or "")
        for character, state in (data.get("characters") or {}).items():
            if character not in self.ids or not isinstance(state, dict):
                continue
            self.where[character] = str(state.get("area") or "")
            self.hears[character] = {str(h) for h in state.get("hears") or []}
            self.intent[character] = str(state.get("intent") or "")
            self.doing[character] = str(state.get("doing") or "")
            self.holding[character] = str(state.get("holding") or "")
        world = data.get("world") or {}
        self.music = str(world.get("music") or "")
        self.cake = bool(world.get("cake"))
        self.user_holding = str(world.get("holding") or "")

    def earshot(self, data: dict[str, Any]) -> None:
        self.earshot_known = True
        self.heard = {str(h) for h in data.get("heard") or [] if str(h) in self.ids}

    def talked_with(self, character: str) -> bool:
        """The user and `character` have spoken to each other: they've met. Whether that's
        new."""
        if character not in self.ids or character in self.talked:
            return False
        self.talked.add(character)
        return True

    # --- Who hears what ----------------------------------------------------------------------------

    def heard_user(self) -> set[str]:
        """Who heard what the player just said: everyone, until the game says otherwise."""
        if not self.earshot_known:
            return set(self.ids)
        return set(self.heard)

    def listeners(self, speaker: str) -> set[str]:
        """Who hears `speaker`'s line: the characters within earshot of them, and USER if the
        player is. Everyone, until the game says where everyone is."""
        if not self.known or speaker not in self.hears:
            return {*self.ids, USER} - {speaker}
        return set(self.hears[speaker])

    def near_user(self) -> set[str]:
        """The characters the player can hear, and so talk with: those whose voice reaches them."""
        if not self.known:
            return set(self.ids)
        return {c for c in self.ids if USER in self.hears.get(c, set())}

    def friends_near_user(self) -> set[str]:
        """Those near the player whom they've talked with: the only ones who'll speak up
        unasked. Someone the player hasn't met waits to be spoken to."""
        return self.near_user() & self.talked

    def situation(self, me: str) -> str:
        """Where `me` is and who's with them, for their LLM, e.g. "You're in the kitchen, with
        Maya. The person is here with you." Empty until the game says where everyone is."""
        if not self.known:
            return ""
        area = self.where.get(me, "")
        within = sorted(self.hears.get(me, set()) - {USER})
        parts = [f"You're in {self.area_name(area)}" if area else "You're in the house"]
        if within:
            parts[0] += ", with " + _names([self.name(c) for c in within])
        parts[0] += "."
        # Where the others are now, which isn't always where they usually are.
        away = [c for c in self.ids if c != me and c not in within]
        if away:
            verb = "isn't" if len(away) == 1 else "aren't"
            parts.append(
                _names([f"{self.name(c)} ({self._in(c)})" for c in away])
                + f" {verb} close enough to hear you."
            )
        if USER in self.hears.get(me, set()):
            if self.user_area and self.user_area == area:
                parts.append("The person is here with you, close enough to talk to.")
            elif self.user_area:
                parts.append(
                    f"The person is in {self.area_name(self.user_area)}, close enough to talk to."
                )
            else:
                parts.append("The person is close enough to talk to.")
            if looking := self._looking(me):
                parts.append(looking)
        else:
            parts.append(
                f"The person is in {self.area_name(self.user_area)}, too far away to hear you."
                if self.user_area
                else "The person isn't close enough to hear you."
            )
        intent = self.intent.get(me, "")
        if intent == "follow":
            parts.append("You're following the person around the house.")
        if self.doing.get(me):
            parts.append(f"You're {self.doing[me]}.")
        if self.holding.get(me):
            parts.append(f"You're holding {self.holding[me]}.")
        if self.user_holding and USER in self.hears.get(me, set()):
            parts.append(f"The person is holding {self.user_holding}.")
        if self.music:
            parts.append(f"Music is playing in {self.area_name(self.music)}.")
        return " ".join(parts)

    def _looking(self, me: str) -> str:
        """What the person's looking at, for `me`, and what they looked at just before: only so
        they know what the person means by "this" or "those", and not something to talk about.
        (Nothing, if they're only looking at `me`.)"""

        def called(seen: str) -> str:
            return "you" if seen == me else self.name(seen) if seen in self.ids else seen

        parts = []
        if self.user_looking_at and self.user_also_seeing:
            parts.append(
                f"The person is looking at {called(self.user_looking_at)}, with "
                f"{self.user_also_seeing} in view."
            )
        elif self.user_looking_at and self.user_looking_at != me:
            parts.append(f"The person is looking at {called(self.user_looking_at)}.")
        if self.user_looked_at and self.user_looked_at != self.user_looking_at:
            parts.append(f"Just before, they were looking at {called(self.user_looked_at)}.")
        if not parts:
            return ""
        return " ".join(parts) + f" {NOTE_LOOKING_ONLY}"

    def _in(self, character: str) -> str:
        """Where `character` is now, e.g. "in the kitchen"."""
        area = self.where.get(character, "")
        return f"in {self.area_name(area)}" if area else "somewhere in the house"

    def for_jev(self) -> dict[str, Any]:
        """Where everyone is, and whom the user has met, for Jev's state."""
        if not self.known:
            return {}
        state: dict[str, Any] = {
            "where": {
                "the user": self.area_name(self.user_area),
                **{self.name(c): self.area_name(self.where.get(c, "")) for c in self.ids},
            },
            "with_the_user": [self.name(c) for c in sorted(self.near_user())] or "nobody",
            "user_has_talked_with": [self.name(c) for c in self.ids if c in self.talked]
            or "nobody yet",
        }
        strangers = [self.name(c) for c in self.ids if c not in self.talked]
        if strangers:
            state["user_has_not_met"] = strangers
        doing = {self.name(c): self.doing[c] for c in self.ids if self.doing.get(c)}
        if doing:
            state["doing"] = doing
        holding = {self.name(c): self.holding[c] for c in self.ids if self.holding.get(c)}
        if holding:
            state["holding"] = holding
        if self.music:
            state["music"] = f"playing in {self.area_name(self.music)}"
        return state


def _names(labels: Sequence[str]) -> str:
    if len(labels) <= 1:
        return "".join(labels)
    return f"{', '.join(labels[:-1])} and {labels[-1]}"
