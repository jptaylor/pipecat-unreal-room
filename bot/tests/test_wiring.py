"""The workers and the room's pipeline can be built (no network: nothing is started)."""

from __future__ import annotations

from pipecat.bus.messages import BusFrameMessage
from pipecat.frames.frames import TTSAudioRawFrame
from pipecat.processors.frame_processor import FrameDirection

import services
from cast import CharacterWorker, prompt
from config import Settings, load_cast
from director import Director
from floor import CastBridge

CAST = load_cast()
SETTINGS = Settings(
    llm_api_key="k",
    llm_base_url=None,
    llm_model="m",
    phonellm=False,
    jev_api_key="k",
    jev_model="jev",
    cartesia_api_key="k",
    deepgram_api_key="k",
)


def test_a_character_is_a_worker_with_its_own_llm_and_voice() -> None:
    maya = CAST[0]
    worker = CharacterWorker(
        maya, services.llm(SETTINGS, prompt(maya, CAST)), services.tts(SETTINGS, maya)
    )
    assert worker.name == maya.id and worker.active
    # It takes its lines as jobs, never frames off the bus.
    frame = BusFrameMessage(
        source="room", frame=TTSAudioRawFrame(b"", 16000, 1), direction=FrameDirection.DOWNSTREAM
    )
    assert not worker.accepts_bus_message(frame)


def test_each_voice_goes_to_its_own_destination() -> None:
    assert [services.tts(SETTINGS, c)._transport_destination for c in CAST] == [c.id for c in CAST]


def test_the_room_has_a_floor_and_a_bridge() -> None:
    director = Director(CAST, referee=None)  # type: ignore[arg-type]
    assert director.floor.gate() is not None and director.floor.ear() is not None
    CastBridge([c.id for c in CAST], bus=None, worker_name="room")  # type: ignore[arg-type]


class Writer:
    def __init__(self) -> None:
        self.text: str | None = None
        self.take: int | None = 1

    def written(self, text: str) -> None:
        self.text = text


async def run_script(chunks: list[str]) -> tuple[list[str], str | None]:
    """What a Script passes on to the TTS, and what it reports as written."""
    from pipecat.frames.frames import (
        LLMFullResponseEndFrame,
        LLMFullResponseStartFrame,
        LLMTextFrame,
    )

    from cast import Script

    writer = Writer()
    script = Script(writer, "juno")  # type: ignore[arg-type]
    passed: list[str] = []

    async def push(frame, direction=FrameDirection.DOWNSTREAM):
        if isinstance(frame, LLMTextFrame):
            passed.append(frame.text)

    script.push_frame = push  # type: ignore[method-assign]
    frames = [LLMFullResponseStartFrame(), *(LLMTextFrame(c) for c in chunks)]
    for frame in [*frames, LLMFullResponseEndFrame()]:
        await script.process_frame(frame, FrameDirection.DOWNSTREAM)
    return passed, writer.text


async def test_silence_never_reaches_the_tts() -> None:
    passed, written = await run_script(["[", "sil", "ent", "]"])
    assert passed == [] and written == "[silent]"
    passed, written = await run_script(["I", " would", " love", " some!"])
    assert passed == ["I", " would", " love", " some!"] and written == "I would love some!"


async def test_what_a_character_does_isnt_said() -> None:
    passed, written = await run_script(["Coming up! *Sk", "ips happily* Let", "'s dance."])
    assert passed == ["Coming up! ", " Let", "'s dance."] and written == "Coming up! Let's dance."
    passed, written = await run_script(["*", "waves*"])
    assert passed == [] and written == ""


async def test_a_character_writes_and_voices_a_line_as_a_job(monkeypatch) -> None:
    """`speak` end to end, with the pipeline and the bus faked: the line is reported once it's
    written, and the job answered with how many runs of TTS audio it had."""
    from pipecat.bus import BusJobRequestMessage
    from pipecat.frames.frames import LLMContextFrame

    maya = CAST[0]
    worker = CharacterWorker(
        maya, services.llm(SETTINGS, prompt(maya, CAST)), services.tts(SETTINGS, maya)
    )
    sent: list[tuple[str, dict | None]] = []

    async def queue_frame(frame, direction=FrameDirection.DOWNSTREAM):
        if isinstance(frame, LLMContextFrame):  # the LLM writes it, the TTS voices it
            worker.written("Hello there.")
            worker.stopped()

    async def send_job_update(job_id, update=None, **kwargs):
        sent.append(("update", update))

    async def send_job_response(job_id, response=None, **kwargs):
        sent.append(("response", response))

    monkeypatch.setattr(worker, "queue_frame", queue_frame)
    monkeypatch.setattr(worker, "send_job_update", send_job_update)
    monkeypatch.setattr(worker, "send_job_response", send_job_response)
    request = BusJobRequestMessage(
        source="room",
        target=maya.id,
        job_id="j1",
        job_name="speak",
        payload={"take": 7, "messages": [{"role": "user", "content": "[User] Hi!"}]},
    )
    worker.active_jobs["j1"] = request
    await worker.speak(request)
    assert sent == [
        ("update", {"text": "Hello there."}),
        ("response", {"text": "Hello there.", "stops": 1}),
    ]
    assert worker.take is None


async def test_a_job_cancelled_before_it_arrives_is_skipped(monkeypatch) -> None:
    from pipecat.bus import BusJobCancelMessage, BusJobRequestMessage

    maya = CAST[0]
    worker = CharacterWorker(
        maya, services.llm(SETTINGS, prompt(maya, CAST)), services.tts(SETTINGS, maya)
    )
    queued: list = []
    responses: list = []

    async def queue_frame(frame, direction=FrameDirection.DOWNSTREAM):
        queued.append(frame)

    async def send_job_response(job_id, response=None, **kwargs):
        responses.append(kwargs.get("status"))

    monkeypatch.setattr(worker, "queue_frame", queue_frame)
    monkeypatch.setattr(worker, "send_job_response", send_job_response)
    # The cancel overtakes its request on the bus.
    await worker.on_bus_message(BusJobCancelMessage(source="room", target=maya.id, job_id="j2"))
    request = BusJobRequestMessage(
        source="room", target=maya.id, job_id="j2", job_name="speak", payload={"take": 8}
    )
    await worker.speak(request)
    assert queued == [] and len(responses) == 1  # never written, just answered as cancelled


async def test_the_end_of_a_line_cut_short_isnt_taken_for_the_next(monkeypatch) -> None:
    """A line abandoned mid-write ends (its LLM stream closes) after the next line's job has
    begun: that end mustn't be reported as the new line, written as nothing."""
    from pipecat.frames.frames import (
        InterruptionFrame,
        LLMFullResponseEndFrame,
        LLMFullResponseStartFrame,
        LLMTextFrame,
    )
    from pipecat.processors.frame_processor import FrameProcessor

    from cast import Script

    async def nothing(*args, **kwargs):
        pass

    # Outside a running pipeline, the base class's own handling of an interruption has nothing
    # to act on: only the Script's is under test.
    monkeypatch.setattr(FrameProcessor, "process_frame", nothing)
    writer = Writer()
    script = Script(writer, "juno")  # type: ignore[arg-type]
    script.push_frame = nothing  # type: ignore[method-assign]
    writer.take = 27
    await script.process_frame(LLMFullResponseStartFrame(), FrameDirection.DOWNSTREAM)
    await script.process_frame(LLMTextFrame("I'd love"), FrameDirection.DOWNSTREAM)
    await script.process_frame(InterruptionFrame(), FrameDirection.DOWNSTREAM)  # take 27 dropped
    writer.take = 33  # the next job has begun
    await script.process_frame(LLMFullResponseEndFrame(), FrameDirection.DOWNSTREAM)  # 27's end
    assert writer.text is None
    await script.process_frame(LLMFullResponseStartFrame(), FrameDirection.DOWNSTREAM)
    await script.process_frame(LLMTextFrame("The sea? Always."), FrameDirection.DOWNSTREAM)
    await script.process_frame(LLMFullResponseEndFrame(), FrameDirection.DOWNSTREAM)
    assert writer.text == "The sea? Always."
