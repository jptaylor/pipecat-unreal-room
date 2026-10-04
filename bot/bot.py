"""Three characters in a house, in an Unreal Engine game. Run with `uv run bot.py -t websocket`
(the game starts a session through the runner's /start, and connects to it over a WebSocket).

The room, and a worker per character, share one runner and its bus:

    room     transport → Deepgram Flux → Hearing → user aggregator → Router → CastBridge
             → FloorGate → transport → FloorEar
    maya, theo, juno   a CharacterWorker each: an LLM with their prompt → Cartesia in their voice

It's the kitchen table's conversation (one pipeline, a voice each, Jev reading every turn), in a
house: the player walks from room to room, and only the characters close enough hear what they
say (the game tells the bot who did, `space.py`). Each character's voice goes to the game on its
own channel (`mixer.py`), so several can talk at once, each from where they stand. Jev reads every
turn (`room.py`), the engine decides what they do (`engine.py`), the director plays it out
(`director.py`), and the floor decides when each line plays (`floor.py`). The game is told who's
speaking and what they say, how they feel and what they do with their hands, and where they go.
The session ends when the game leaves.
"""

from __future__ import annotations

import asyncio
import os
import sys
import threading
import uuid

from loguru import logger
from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.classifiers.jev.classifier import JevClassifier
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.worker import PipelineParams, PipelineWorker
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import (
    LLMUserAggregator,
    LLMUserAggregatorParams,
)
from pipecat.runner.types import RunnerArguments, WebSocketRunnerArguments
from pipecat.serializers.protobuf import ProtobufFrameSerializer
from pipecat.transports.base_transport import BaseTransport
from pipecat.transports.websocket.fastapi import FastAPIWebsocketParams
from pipecat.workers.runner import WorkerRunner

import services
from cast import CharacterWorker, prompt
from config import (
    LULL_WAITS_S,
    ROOM,
    Settings,
    load_cast,
    load_environment,
)
from director import Director
from floor import CastBridge
from mixer import CastWebsocketTransport
from room import Referee

load_environment()
CAST = load_cast()


def transport_params() -> FastAPIWebsocketParams:
    # A transport destination per character, each a channel of the game's audio.
    return FastAPIWebsocketParams(
        audio_in_enabled=True,
        audio_out_enabled=True,
        audio_out_destinations=[c.id for c in CAST],
        add_wav_header=False,
        serializer=ProtobufFrameSerializer(),
    )


async def run_bot(transport: BaseTransport, runner_args: RunnerArguments) -> None:
    settings = Settings.from_env()
    ids = [c.id for c in CAST]
    runner = WorkerRunner(handle_sigint=runner_args.handle_sigint)
    warming = asyncio.create_task(services.warm_llm(settings))

    jev = services.jev(settings)
    director = Director(CAST, Referee(JevClassifier(client=jev), CAST))

    user = LLMUserAggregator(
        LLMContext(),
        params=LLMUserAggregatorParams(
            # Not for turns (Flux decides those): the director hears from it when the user's
            # voice starts and stops, so that no line starts over them.
            vad_analyzer=SileroVADAnalyzer(),
            user_turn_strategies=services.turn_strategies(),
            user_idle_timeout=LULL_WAITS_S[0],
        ),
    )
    pipeline = Pipeline(
        [
            transport.input(),
            services.stt(settings, CAST),
            director.hearing(),
            user,
            director.router(),
            CastBridge(ids, bus=runner.bus, worker_name=ROOM),
            director.floor.gate(),
            transport.output(),
            director.floor.ear(),
        ]
    )
    room = PipelineWorker(
        pipeline,
        name=ROOM,
        params=PipelineParams(enable_metrics=True, enable_usage_metrics=True),
        idle_timeout_secs=runner_args.pipeline_idle_timeout_secs,
    )
    director.worker = room

    @user.event_handler("on_user_turn_idle")
    async def on_user_turn_idle(aggregator):
        director.spawn(director.idle(), "idle")

    # The game is told who's who once it's listening and every worker has started.
    ready: set[str] = set()

    async def ready_for(what: str) -> None:
        ready.add(what)
        if ready == {"client", "workers"}:
            await director.welcome()

    @room.rtvi.event_handler("on_client_ready")
    async def on_client_ready(rtvi):
        await ready_for("client")

    @room.rtvi.event_handler("on_client_message")
    async def on_client_message(rtvi, message):
        data = message.data if isinstance(message.data, dict) else {}
        if message.type == "world":
            director.world(data)
        elif message.type == "space":
            director.update_space(data)
        elif message.type == "earshot":
            director.earshot(data)
        elif message.type == "met":
            director.spawn(director.met(str(data.get("who"))), "met")
        elif message.type == "event":
            director.spawn(director.event(data), "event")
        elif message.type == "host":
            director.spawn(director.host(str(data.get("action"))), "host")

    @runner.event_handler("on_ready")
    async def on_ready(runner):
        await ready_for("workers")

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        logger.info("Session: the game connected")

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info("Session: the game left")
        await runner.cancel()

    # However the room ends (the game leaves, it's idle, it fails to start, an EndFrame), the
    # characters go with it: they'd otherwise keep their LLM and TTS connections open for good.
    @room.event_handler("on_pipeline_finished")
    async def on_pipeline_finished(worker, frame):
        await runner.cancel()

    logger.info(
        f"Session: {', '.join(c.name for c in CAST)} on {settings.llm_model}, "
        f"Jev {settings.jev_model}"
    )
    try:
        try:
            await jev.connect()
        except Exception as error:  # noqa: BLE001 — the reads fall back; the session goes on
            logger.warning(f"Session: Jev isn't reachable ({error}); routing will fall back")
        await runner.add_workers(
            *(
                CharacterWorker(
                    c, services.llm(settings, prompt(c, CAST)), services.tts(settings, c)
                )
                for c in CAST
            ),
            room,
        )
        await runner.run()
    finally:
        warming.cancel()
        await director.close()
        await jev.close()
        referee = director.referee
        logger.info(f"Session: ended (Jev asked {referee.asked}, {referee.cached} from the cache)")


async def bot(runner_args: RunnerArguments) -> None:
    """The entry point (the dev runner's): one session per game. Every log line of the session
    carries its id."""
    configure_logging(verbose=bool(getattr(getattr(runner_args, "cli_args", None), "verbose", 0)))
    session = (getattr(runner_args, "session_id", None) or uuid.uuid4().hex)[:8]
    with logger.contextualize(session=session):
        if not isinstance(runner_args, WebSocketRunnerArguments):
            raise RuntimeError("the game connects over a WebSocket: run with -t websocket")
        transport = CastWebsocketTransport(
            runner_args.websocket, transport_params(), channels=[c.id for c in CAST]
        )
        await run_bot(transport, runner_args)


_logging_configured = False


def configure_logging(*, verbose: bool) -> None:
    """Once per process: BOT_LOG_LEVEL (INFO by default; DEBUG adds what was said), with the
    session id on every line."""
    global _logging_configured
    if _logging_configured:
        return
    _logging_configured = True
    logger.configure(extra={"session": "-"})
    if verbose:
        return
    logger.remove()
    logger.add(
        sys.stderr,
        level=os.getenv("BOT_LOG_LEVEL", "INFO").upper(),
        format=(
            "<green>{time:HH:mm:ss.SSS}</green> | <level>{level: <7}</level> | "
            "<cyan>{extra[session]}</cyan> | {name}:{line} - <level>{message}</level>"
        ),
    )


if __name__ == "__main__":
    # The runner's banner, and the logs, aren't all ASCII: a Windows console, or a log file,
    # may not take them in its own encoding.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
    # The runner loads dotenv with override=True; explicitly set variables must win.
    configured = dict(os.environ)
    from pipecat.runner.run import main

    os.environ.update(configured)
    settings = Settings.from_env()  # a missing key fails now, not when the game first connects
    # A hosted PhoneLLM scales to zero; start waking it before anyone connects.
    threading.Thread(target=lambda: asyncio.run(services.warm_llm(settings)), daemon=True).start()
    main()
