# Pipecat Room

Walk around a house and talk to the three people who live there. This is a
[Pipecat](https://github.com/pipecat-ai/pipecat) voice agent in an Unreal Engine
game: one conversation, three characters, each with their own voice, coming
from where they stand.

- **Maya**, a botanist, is in the conservatory.
- **Theo**, a chef, is in the kitchen.
- **Juno**, a musician, is in the music room.

Nobody speaks to you until you speak to them, and nobody tells you their name
until you ask. As you speak, a ring around you shows how far your voice
carries. Speak up and it carries further; through a wall it carries half as
far. Only the people inside it hear you. Call Maya from the hall and she won't
hear you, but Theo might, if he's close, and say so.

Talk to whoever's near: one of them by name, or everyone at once ("who wants
cake?"). They answer, talk among themselves, tease each other and react, just
like [the kitchen table](https://github.com/jptaylor/pipecat-one-pipeline-two-characters)
this is built on, but only with whoever's close enough to hear. Ask them to
move, and they do:

| Say                                        | They                             |
| ------------------------------------------ | -------------------------------- |
| "Follow me", "Come with me"                | follow you around the house      |
| "Wait here", "Stop following me"           | stay where they are              |
| "Come over here", "Join us"                | walk over to you                 |
| "Go to the kitchen", "Meet me in the hall" | go there                         |
| "Go back", "Back to your plants"           | go back to their own room        |

As they speak and listen, their bodies show how they feel (happy, laughing,
proud, surprised, thinking, worried, sad, angry, bored or doubtful) and what
they do with their hands and heads (nod, shake, shrug, wave, point, clap,
beckon, bow, facepalm, laugh). Jev reads every line for it, as the WoW companion
picks its portrait's face.

There's plenty to do in the house. Walk up to something and, when its prompt
shows in the corner, press **E** (or X on an Xbox pad, square on a DualSense):

| Where            | What                                                                        |
| ---------------- | --------------------------------------------------------------------------- |
| The hall         | Put a record on the gramophone. Whoever's near dances, and the lamps party. |
| The music room   | Plink a few notes on the piano.                                             |
| The kitchen      | Ring the dinner bell, and everyone comes. Take a slice of cake, if there is one. |
| The conservatory | Pick a flower or a tomato (Maya notices), or toss a coin in the fountain and make a wish. |
| The gallery      | Give a sculpture a spin.                                                    |

Holding something, press E by someone to give it to them (food, they eat), or
on your own to eat it or put it down.

Ask them to do things, too:

| Say                                        | They                                      |
| ------------------------------------------ | ----------------------------------------- |
| "Play us something"                        | play the piano                            |
| "Put a record on", "Turn the music off"    | go to the gramophone                      |
| "Let's dance!", "You can stop now"         | dance, or stop                            |
| "Bake a cake"                              | bake one at the stove                     |
| "I'm starving", "Could I have some cake?"  | bring you a slice, baking one first if they have to (Maya brings a tomato) |
| "Pick me a flower"                         | bring you one from the conservatory       |
| "Water the plants"                         | water the trees, which grow               |
| "Can I have that?"                         | hand you what they're holding             |

They do things of their own accord, too: say they'll dance, or put a record on,
and they do. And while you're elsewhere, they get on with their day. Theo
cooks, and bakes when there's no cake. Maya waters the trees and tends the
flowers. Juno plays the piano. Now and then, one drops in on another for a chat.
Whenever something happens (you give them something, a cake's ready, the bell
rings, the music starts), whoever saw or heard it knows, and whoever it's most
about says something.

## 📋 Requirements

- Windows, on x86_64, with a GPU that has ray tracing hardware (it looks its
  best on an NVIDIA RTX card).
- Unreal Engine 5.8, from the Epic Games Launcher.
- [Visual Studio](https://visualstudio.microsoft.com/downloads/) or its Build
  Tools, with "Desktop development with C++", "C++ CMake tools for Windows" and
  the ".NET Framework 4.8 SDK".
- The [Pipecat C++ client](https://github.com/pipecat-ai/pipecat-client-cxx)'s
  source, to build it for the plugin.
- [uv](https://docs.astral.sh/uv/), to run the bot.
- API keys for [OpenAI](https://platform.openai.com/api-keys),
  [Cartesia](https://play.cartesia.ai/keys),
  [Deepgram](https://console.deepgram.com) (Flux) and Jev (TypeSafe).
- A microphone, and **headphones**: the game doesn't cancel echo, so with
  speakers the characters hear themselves.
- Optionally, NVIDIA's DLSS 4.5 plugin for Unreal Engine 5.8, for DLSS, Frame
  Generation and Reflex: unzip its `DLSS` and `Streamline*` plugins into
  `Plugins/`. Without them, the game uses Unreal's TSR.

## 🔨 Build

In PowerShell, from this directory:

```
$env:UE_ROOT = "C:\Program Files\Epic Games\UE_5.8"
$env:PIPECAT_CLIENT_CXX = "C:\path\to\pipecat-client-cxx"
```

1. Build the Pipecat C++ client and its Daily and WebSocket transports for the
   plugin (this downloads Daily's Core C++ SDK the first time)
   (into `Plugins\Pipecat\ThirdParty\Win64`):

   ```
   .\Plugins\Pipecat\ThirdParty\build-windows.ps1
   ```

2. Build the game:

   ```
   & "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" PipecatRoomEditor Win64 Development -Project="$PWD\PipecatRoom.uproject"
   ```

3. Set up its content. This copies Unreal Engine's Third Person template (the
   player, the mannequins and their animations, and the input) into `Content\`,
   and creates the game's two materials in the editor:

   ```
   .\setup.ps1
   ```

In cmd.exe, use `set` for the variables, and the `.bat` next to each script.

## 🚀 Run

1. Start the bot, with your API keys in `bot\.env` (`DAILY_API_KEY` too). The
   game and the bot meet in a Daily room, where each character's voice is an
   audio track of its own, so they can all talk at once. The bot's Daily
   transport (daily-python) is for Linux and macOS, so on Windows it runs in
   WSL (`wsl --install`, once):

   ```
   cd bot
   copy env.example .env   # then set the API keys in it
   wsl bash ./run-wsl.sh
   ```

   It waits for the game at `http://localhost:7860`. Or, without WSL, run the
   bot over a WebSocket with `uv run bot.py -t websocket`, and start the game
   with `-PipecatTransport=websocket` (or set `Transport=websocket` in
   `Config/DefaultGame.ini`).

2. In another terminal, open the game in the editor and press Play, or run it
   on its own:

   ```
   & "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor.exe" "$PWD\PipecatRoom.uproject" -game
   ```

You start in the hall. Walk with WASD and the mouse, or a controller (an Xbox
pad, or a DualSense over Bluetooth or USB). Go through a doorway, find someone,
and say hello.

## ⚙️ How it works

```
the game  ── microphone ──►  room      transport → Deepgram Flux → Hearing → user aggregator
          ◄─ 3-channel ────            → Router → CastBridge → FloorGate → transport → FloorEar
             audio, one           maya, theo, juno   a worker each: OpenAI → Cartesia, in their voice
             voice each
          ── who heard what, where everyone is ──►  the director (Jev reads every turn)
          ◄─ who says what, moods and gestures, moves ──
```

- **The bot** (`bot/`) is the kitchen table's one pipeline with a worker per
  character, for three. Jev reads every turn: who it's for, a group or one, all
  at once or in turn, carry on or hush. The engine decides who talks, and for
  how long they talk among themselves. The floor decides when each line plays.
  See `bot/director.py`, `engine.py`, `floor.py` and `room.py`. It adds the
  house:
  - **Hearing** (`space.py`). The game tells the bot who heard what you said,
    and who can hear each character. Only they answer, and each character's
    LLM is only shown what they heard. If you speak to someone too far away,
    someone who did hear may say so.
  - **Moving.** Jev also reads whether you're asking whoever you're talking to
    to follow you, wait, come over, or go somewhere. Their line is written
    knowing it, and the game moves them (`move`).
  - **Moods and gestures.** For each line, Jev reads the speaker's mood and
    gesture, and how the others hearing it take it. For what you say, it reads
    how your listeners take it, even before you finish. The game poses them
    (`emote`).
  - **Doing things.** Jev reads whether you're asking them to do something
    (dance, play, bring you cake…), and for each of their lines, whether
    they're setting about something themselves. The game has them do it (`act`).
  - **What happens** (`event`). The game tells the bot what happens in the
    house, and who saw or heard it. They know it, and whoever it's most about
    says something, as soon as nobody's talking. The game also tells the bot
    what each of them is doing and holding, and whether there's music or cake.
  - **Meeting.** Someone you haven't talked with yet waits for you to speak
    first, and only those you've talked with pick up a quiet moment. Jev knows
    who you've met, so "oh, hello, who are you?" goes to the stranger, not to
    the friend beside them.
  - **Their voices** go to the game each on its own: over Daily, as an audio
    track per character (a transport destination each), which the game reads
    as a channel each; over a WebSocket, as one stream with a channel per
    character, paced in real time (`mixer.py`). Either way, voices said at
    once play at once, each from its character's head.
- **The game** (`Source/PipecatRoom`):
  - `RoomHouse` builds the house: walls with doorways and windows, flat colors,
    the sun through the windows and skylights, lamps that swing, stage lights
    that sweep, all casting shadows. It also finds the way between rooms,
    and around the furniture and whoever's standing about.
  - `RoomStage` connects to the bot. It puts each voice channel in its
    character's head (muffled through walls), works out who hears you and who
    hears whom, and draws your voice's reach.
  - `RoomCharacter` walks, follows, waits and looks at whoever's talking, and
    does jobs: a walk somewhere, a while busy, something picked up or handed
    over.
  - `RoomThings` is the house's things: the gramophone and the piano (with a
    little synthesizer for each, `RoomMusic`), the stove and the cake, the bell,
    the flower and tomato beds, the fountain and the sculptures.
  - `RoomStageLife` is what the player and the characters do with them, and
    their days: routines, visits, and dancing to the music when it's on.
  - `RoomRig` poses each character's mood, gestures and talking hands on top
    of the mannequin's idle and walk, with two-bone IK for the arms. It also
    poses what they do (playing the piano, stirring a pot, watering, holding,
    offering, eating) and dancing to the beat, with their feet kept planted.
    It's a procedural rig, so it needs no animation assets.
  - `RoomVoiceRing` draws the waveform at your feet, the edge of your voice's
    reach, and ripples running out to it.
- **The plugin** (`Plugins/Pipecat`) is the demo's, with several voice channels,
  each played from where it's attached, messages to the bot, and the
  microphone's waveform.

## 🎛️ Options

On the command line, with the editor too:

- `-PipecatStartUrl=URL`: the bot's start endpoint (`Config/DefaultGame.ini` has
  `http://localhost:7860/start`).
- `-PipecatApiKey=KEY`, or `PIPECAT_API_KEY`: a key for starting the bot, e.g. on
  Pipecat Cloud.
- `-PipecatNoMicrophone`, `-PipecatSay="..."`: to try it without speaking.
- `-PipecatVolume=0`: the voices play silently.
- `-PipecatNoLogo`: skips Pipecat's logo.

In the console (`` ` ``):

- `Room.Say Maya, can you follow me?`: say something, as if spoken at your
  usual loudness, where you are.
- `Room.Speak 3 6`: pretend you speak for 3 s, 6 dB louder than usual, to see
  the ring.
- `Room.Emote theo thinking nod`, `Room.Move juno go kitchen`: pose or move
  someone.
- `Room.Teleport kitchen`: go somewhere.
- `Room.Act theo food`: have someone do something (`dance`, `play`, `music_on`,
  `music_off`, `cook`, `food`, `flower`, `water`, `stop`, `hand`).
- `Room.Interact`: do whatever you can where you are, as if you pressed E.
- `Room.Routine maya`, `Room.Routine juno theo`: have someone get on with their
  day now, or visit someone.
- `room.MusicVolume 0.5`: how loud the house's music and sounds are.

`Config/DefaultGame.ini` has how quiet your voice can be (`SpeechMinDb`), how
far voices carry (`NormalRange`, `MaxRange`, `ThroughWalls`, `CharacterRange`),
and how the characters' voices fade with distance and through walls
(`VoiceFullWithin`, `VoiceFadesOver`, `VoiceThroughWalls`). `bot/characters.json`
has who the characters are: their names, colors, voices, mannequins and rooms.
The game reads it too. Each character's persona is in `bot/prompts/<id>.md`, and the
house they share is in `bot/prompts/character.md`.

To use PhoneLLM instead of OpenAI, as the kitchen table does, set
`PHONELLM_API_KEY`, `PHONELLM_BASE_URL` and `LLM_MODEL` in `bot\.env`.

## ✅ Checks

```
cd bot
uv run ruff check . ; uv run pyright ; uv run pytest -q
uv run python scripts/converse.py      # the conversation in text, without the house
```

## 📝 Notes

- One conversation at a time: the characters can split up, but they don't hold
  two separate conversations at once.
- The emotes are posed procedurally on the template's mannequin. For richer
  ones, animation packs for the UE5 mannequin on [Fab](https://www.fab.com) can
  replace the rig's poses, mood by mood.
- Rendering: Lumen with hardware ray tracing (and hit-lit reflections),
  MegaLights for the many shadowed lamps, Virtual Shadow Maps, volumetric fog
  for the light shafts, DLSS 4.5 with Frame Generation and Reflex, and HDR where
  the display has it. At 4K on an RTX 4090 it renders a frame in about 12.5 ms
  (DLSS Performance), and runs at a steady 60 fps.

## 📄 License

BSD 2-Clause: see [LICENSE](LICENSE).
