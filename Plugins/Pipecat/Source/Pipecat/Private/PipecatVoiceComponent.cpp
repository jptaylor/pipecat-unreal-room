//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "PipecatVoiceComponent.h"

#include "PipecatMicrophone.h"

#include "Async/Async.h"
#include "Components/AudioComponent.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundWaveProcedural.h"

THIRD_PARTY_INCLUDES_START
#include <pipecat/pipecat.h>
#include <pipecat/websocket/transport.h>
#if PIPECAT_WITH_DAILY
#include <pipecat/daily/transport.h>
#endif
THIRD_PARTY_INCLUDES_END

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogPipecat, Log, All);

namespace
{
// The microphone is sent at 16 kHz, like most speech-to-text services use, and
// the bot's voice is played at 48 kHz.
const uint32 UserSampleRate = 16000;
const uint32 BotSampleRate = 48000;
const int32 MaxVoiceChannels = 8;

// The most bot voice Unreal has waiting to be played. The bot sends its voice
// faster than it plays, so it's read as Unreal needs it.
const int32 MaxQueuedBotVoiceBytes = BotSampleRate * 3 / 10 * sizeof(int16);

FString ToFString(const std::string& Text)
{
	return FString(UTF8_TO_TCHAR(Text.c_str()));
}

std::string ToStdString(const FString& Text)
{
	return std::string(TCHAR_TO_UTF8(*Text));
}

// How loud one channel of interleaved frames is, from 0 to 1.
float Loudness(const int16* Frames, int32 NumFrames, int32 Channels, int32 Channel)
{
	double Sum = 0.0;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const double Sample = Frames[i * Channels + Channel];
		Sum += Sample * Sample;
	}
	float Rms = NumFrames > 0 ? static_cast<float>(std::sqrt(Sum / NumFrames) / 32768.0) : 0.0f;
	return FMath::Clamp(Rms * 4.0f, 0.0f, 1.0f);
}
} // namespace

// Everything that talks to Pipecat. Pipecat calls its callbacks on the client's
// own thread, and they're passed on to the component on the game thread.
class FPipecatSession : public pipecat::PipecatClientCallbacks
{
public:
	FPipecatSession(
		UPipecatVoiceComponent* InOwner,
		const TArray<USoundWaveProcedural*>& InWaves,
		const TMap<FString, FPipecatFunctionHandler>& InFunctions,
		EPipecatTransport InTransport,
		const TArray<FString>& Tracks)
		: Owner(InOwner), Waves(InWaves), Transport(InTransport)
	{
		for (const TPair<FString, FPipecatFunctionHandler>& Function : InFunctions)
		{
			Functions.emplace(ToStdString(Function.Key), Function.Value);
		}
		for (std::atomic<float>& Level : Levels)
		{
			Level = 0.0f;
		}

		const uint8_t Channels = static_cast<uint8_t>(FMath::Clamp(Waves.Num(), 1, MaxVoiceChannels));
		pipecat::PipecatClientOptions Options;
#if PIPECAT_WITH_DAILY
		if (Transport == EPipecatTransport::Daily)
		{
			// Each voice channel is one of the bot's tracks, if it has several.
			pipecat::DailyTransportOptions TransportOptions;
			TransportOptions.user_audio_sample_rate = UserSampleRate;
			TransportOptions.user_audio_channels = 1;
			TransportOptions.bot_audio_sample_rate = BotSampleRate;
			TransportOptions.bot_audio_channels = Channels;
			for (int32 Index = 0; Index < FMath::Min(Tracks.Num(), static_cast<int32>(Channels)); ++Index)
			{
				TransportOptions.bot_audio_tracks.push_back(ToStdString(Tracks[Index]));
			}
			Options.transport = std::make_unique<pipecat::DailyTransport>(TransportOptions);
		}
#else
		if (Transport == EPipecatTransport::Daily)
		{
			UE_LOG(LogPipecat, Warning, TEXT("This build has no Daily transport: using the WebSocket"));
			Transport = EPipecatTransport::WebSocket;
		}
#endif
		if (!Options.transport)
		{
			pipecat::WebSocketTransportOptions TransportOptions;
			TransportOptions.user_audio_sample_rate = UserSampleRate;
			TransportOptions.user_audio_channels = 1;
			TransportOptions.bot_audio_sample_rate = BotSampleRate;
			TransportOptions.bot_audio_channels = Channels;
			Options.transport = std::make_unique<pipecat::WebSocketTransport>(TransportOptions);
		}
		Options.callbacks = this;
		Client = std::make_unique<pipecat::PipecatClient>(std::move(Options));

		Running = true;
		Reader = std::thread([this] { PlayBotVoice(); });
	}

	virtual ~FPipecatSession() override
	{
		Stop();
	}

	// Captures the microphone, and sends it to the bot whenever it's
	// connected: as it is, or gated, `GateDelay` seconds late.
	void StartMicrophone(bool bGate, float GateDelay, const FString& Device)
	{
		if (bGate)
		{
			Delay.Init(0, FMath::Max(FMath::RoundToInt(GateDelay * UserSampleRate), 1));
			DelayAt = 0;
		}
		Microphone.Start(UserSampleRate, [this, bGate](const int16* Frames, int32 NumFrames) {
			if (!Running)
			{
				return;
			}
			if (!bGate)
			{
				Client->send_user_audio(Frames, NumFrames);
				return;
			}
			// A moment late, and faded in and out (over 5 ms) as the player
			// starts and stops speaking. Only this thread touches the delay.
			const float Target = MicrophoneOpen ? 1.0f : 0.0f;
			const float Step = 1.0f / (0.005f * UserSampleRate);
			Gated.SetNumUninitialized(NumFrames, EAllowShrinking::No);
			for (int32 i = 0; i < NumFrames; ++i)
			{
				const int16 Late = Delay[DelayAt];
				Delay[DelayAt] = Frames[i];
				DelayAt = (DelayAt + 1) % Delay.Num();
				Gain += FMath::Clamp(Target - Gain, -Step, Step);
				Gated[i] = static_cast<int16>(Late * Gain);
			}
			Client->send_user_audio(Gated.GetData(), NumFrames);
		}, Device);
	}

	void SetMicrophoneOpen(bool bOpen)
	{
		MicrophoneOpen = bOpen;
	}

	void Start(const FString& StartUrl, const FString& ApiKey)
	{
		if (ConnectThread.joinable())
		{
			return;
		}

		pipecat::APIRequest Request;
		Request.endpoint = ToStdString(StartUrl);
		// The start endpoint creates a Daily room for the bot and the game to
		// meet in, or gives the bot's WebSocket.
		if (Transport == EPipecatTransport::Daily)
		{
			Request.request_data = {{"createDailyRoom", true}};
		}
		else
		{
			Request.request_data = {{"transport", "websocket"}};
		}
		if (!ApiKey.IsEmpty())
		{
			Request.headers["Authorization"] = "Bearer " + ToStdString(ApiKey);
		}

		// Starting the bot and connecting take a few seconds.
		ConnectThread = std::thread([this, Request] {
			try
			{
				UE_LOG(LogPipecat, Log, TEXT("Starting the bot at %hs"), Request.endpoint.c_str());
				Client->start_bot_and_connect(Request);
			}
			catch (const std::exception& Error)
			{
				if (Running)
				{
					ReportError(FString::Printf(TEXT("Unable to connect: %hs"), Error.what()));
				}
			}
		});
	}

	void Stop()
	{
		if (!Running.exchange(false))
		{
			return;
		}
		// Also stops a connection in progress, and wakes up the reader.
		Client->disconnect();
		if (ConnectThread.joinable())
		{
			ConnectThread.join();
		}
		Microphone.Stop();
		if (Reader.joinable())
		{
			Reader.join();
		}
		Ready = false;
	}

	void SendText(const FString& Text)
	{
		try
		{
			Client->send_text(ToStdString(Text));
		}
		catch (const std::exception& Error)
		{
			ReportError(FString::Printf(TEXT("Unable to send text: %hs"), Error.what()));
		}
	}

	void SendClientMessage(const FString& Type, const FString& DataJson)
	{
		if (!Ready)
		{
			return;
		}
		nlohmann::json Data = nullptr;
		if (!DataJson.IsEmpty())
		{
			Data = nlohmann::json::parse(ToStdString(DataJson), nullptr, false);
			if (Data.is_discarded())
			{
				UE_LOG(LogPipecat, Warning, TEXT("Not sending a %s message: its data isn't JSON"), *Type);
				return;
			}
		}
		try
		{
			Client->send_client_message(ToStdString(Type), Data);
		}
		catch (const std::exception& Error)
		{
			ReportError(FString::Printf(TEXT("Unable to send a message: %hs"), Error.what()));
		}
	}

	bool IsReady() const { return Ready; }

	float GetBotVoiceLevel(int32 Channel) const
	{
		return Channel >= 0 && Channel < MaxVoiceChannels ? Levels[Channel].load() : 0.0f;
	}

	float GetMicrophoneLevel() const { return Microphone.GetLevel(); }
	float GetMicrophoneRms() const { return Microphone.GetRms(); }
	void GetMicrophoneWaveform(TArray<float>& OutSamples, int32 NumSamples) const
	{
		Microphone.GetWaveform(OutSamples, NumSamples);
	}

	//
	// pipecat::PipecatClientCallbacks, on the client's thread.
	//

	virtual void on_bot_ready(const pipecat::rtvi::BotReadyData&) override
	{
		Ready = true;
		Post([](UPipecatVoiceComponent& C) { C.OnBotReady.Broadcast(); });
	}

	virtual void on_disconnected() override
	{
		Ready = false;
		Post([](UPipecatVoiceComponent& C) { C.OnDisconnected.Broadcast(); });
	}

	virtual void on_error(const pipecat::rtvi::ErrorData& Error) override
	{
		ReportError(ToFString(Error.error));
	}

	virtual void on_user_started_speaking() override
	{
		Post([](UPipecatVoiceComponent& C) { C.OnUserStartedSpeaking.Broadcast(); });
	}

	virtual void on_user_stopped_speaking() override
	{
		Post([](UPipecatVoiceComponent& C) { C.OnUserStoppedSpeaking.Broadcast(); });
	}

	virtual void on_user_transcript(const pipecat::rtvi::TranscriptData& Data) override
	{
		FString Text = ToFString(Data.text);
		bool bFinal = Data.final;
		Post([Text, bFinal](UPipecatVoiceComponent& C) { C.OnUserTranscript.Broadcast(Text, bFinal); });
	}

	virtual void on_bot_started_speaking() override
	{
		Post([](UPipecatVoiceComponent& C) { C.OnBotStartedSpeaking.Broadcast(); });
	}

	virtual void on_bot_stopped_speaking() override
	{
		// The bot doesn't send silence, so its voice isn't read until it speaks
		// again.
		for (std::atomic<float>& Level : Levels)
		{
			Level = 0.0f;
		}
		Post([](UPipecatVoiceComponent& C) { C.OnBotStoppedSpeaking.Broadcast(); });
	}

	virtual void on_bot_interrupted() override
	{
		Post([](UPipecatVoiceComponent& C) { C.OnBotInterrupted.Broadcast(); });
	}

	virtual void on_bot_output(const pipecat::rtvi::BotOutputData& Data) override
	{
		// Text the bot speaks comes first when it's written, and then again as
		// it's spoken. Pass it on once, when the bot starts speaking it, so it
		// matches the voice.
		if (Data.will_be_spoken.value_or(false) && Data.spoken_status == std::optional<std::string>("new"))
		{
			return;
		}
		if (Data.segment_id && !OutputSegments.insert(*Data.segment_id).second)
		{
			return;
		}
		FString Text = ToFString(Data.text).TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			// E.g. a paragraph break.
			return;
		}
		Post([Text](UPipecatVoiceComponent& C) { C.OnBotOutput.Broadcast(Text); });
	}

	virtual void on_llm_function_call_in_progress(
		const pipecat::rtvi::LLMFunctionCallInProgressData& Data,
		pipecat::FunctionCallResultCallback /* Respond */) override
	{
		// The bot runs its functions and gives its LLM the results, so the
		// game only reacts to them, and never responds.
		auto Function = Data.function_name ? Functions.find(*Data.function_name) : Functions.end();
		if (Function == Functions.end())
		{
			return;
		}
		FPipecatFunctionHandler Handler = Function->second;
		FString Arguments = ToFString(Data.arguments.dump());
		Post([Handler, Arguments](UPipecatVoiceComponent&) { Handler(Arguments); });
	}

	virtual void on_server_message(const nlohmann::json& Data) override
	{
		FString Message = ToFString(Data.dump());
		Post([Message](UPipecatVoiceComponent& C) { C.OnServerMessage.Broadcast(Message); });
	}

private:
	template <typename F>
	void Post(F&& Fn)
	{
		AsyncTask(ENamedThreads::GameThread, [Owner = Owner, Fn = MoveTemp(Fn)]() {
			if (UPipecatVoiceComponent* Component = Owner.Get())
			{
				Fn(*Component);
			}
		});
	}

	void ReportError(const FString& Error)
	{
		UE_LOG(LogPipecat, Warning, TEXT("%s"), *Error);
		Post([Error](UPipecatVoiceComponent& C) { C.OnError.Broadcast(Error); });
	}

	// Reads the bot's voices, a channel each, and queues each on the sound
	// wave that plays it.
	void PlayBotVoice()
	{
		const int32 Channels = FMath::Clamp(Waves.Num(), 1, MaxVoiceChannels);
		const int32 FramesPerRead = BotSampleRate / 100;
		std::vector<int16> Frames(FramesPerRead * Channels);
		std::vector<int16> Channel(FramesPerRead);
		while (Running)
		{
			if (Waves.Num() == 0 || Waves[0]->GetAvailableAudioByteCount() >= MaxQueuedBotVoiceBytes)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			int32 Read = Client->read_bot_audio(Frames.data(), FramesPerRead);
			if (Read <= 0)
			{
				// Not connected.
				for (std::atomic<float>& Level : Levels)
				{
					Level = 0.0f;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			for (int32 c = 0; c < Channels; ++c)
			{
				Levels[c] = Loudness(Frames.data(), Read, Channels, c);
				if (Channels == 1)
				{
					Waves[c]->QueueAudio(reinterpret_cast<const uint8*>(Frames.data()), Read * sizeof(int16));
					continue;
				}
				for (int32 i = 0; i < Read; ++i)
				{
					Channel[i] = Frames[i * Channels + c];
				}
				Waves[c]->QueueAudio(reinterpret_cast<const uint8*>(Channel.data()), Read * sizeof(int16));
			}
		}
	}

	TWeakObjectPtr<UPipecatVoiceComponent> Owner;
	// Kept alive by the component, which stops the session first.
	TArray<USoundWaveProcedural*> Waves;
	EPipecatTransport Transport;
	std::unique_ptr<pipecat::PipecatClient> Client;
	FPipecatMicrophone Microphone;

	std::atomic<bool> Running {false};
	std::atomic<bool> Ready {false};
	// The gated microphone: whether it's open (set on the game thread), and,
	// on the capture thread, the delay, and how open it is.
	std::atomic<bool> MicrophoneOpen {false};
	TArray<int16> Delay;
	int32 DelayAt = 0;
	float Gain = 0.0f;
	TArray<int16> Gated;
	std::array<std::atomic<float>, MaxVoiceChannels> Levels;
	std::thread ConnectThread;
	std::thread Reader;

	// Only used on the client's thread.
	std::set<int64_t> OutputSegments;
	// The game functions the bot can call, by name.
	std::map<std::string, FPipecatFunctionHandler> Functions;
};

UPipecatVoiceComponent::UPipecatVoiceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UPipecatVoiceComponent::BeginPlay()
{
	Super::BeginPlay();

	// Each of the bot's voices, played from the owner's position until it's
	// attached somewhere else.
	AActor* Owner = GetOwner();
	VoiceChannels = FMath::Clamp(VoiceChannels, 1, MaxVoiceChannels);
	for (int32 Channel = 0; Channel < VoiceChannels; ++Channel)
	{
		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
		Wave->SetSampleRate(BotSampleRate);
		Wave->NumChannels = 1;
		Wave->Duration = INDEFINITELY_LOOPING_DURATION;
		Wave->SoundGroup = SOUNDGROUP_Voice;
		Wave->bLooping = false;
		// Unreal stops sounds it virtualizes, e.g. when they can't be heard,
		// and a stream can't be restarted.
		Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
		VoiceWaves.Add(Wave);

		UAudioComponent* Audio = NewObject<UAudioComponent>(Owner, *FString::Printf(TEXT("PipecatVoice%d"), Channel));
		if (USceneComponent* Root = Owner->GetRootComponent())
		{
			Audio->SetupAttachment(Root);
		}
		Audio->bAutoActivate = false;
		Audio->bAllowSpatialization = true;
		Audio->bOverrideAttenuation = true;
		FSoundAttenuationSettings& Attenuation = Audio->AttenuationOverrides;
		Attenuation.bAttenuate = true;
		Attenuation.bSpatialize = true;
		// Full volume within the inner radius, and fading out after that.
		Attenuation.AttenuationShapeExtents = FVector(VoiceInnerRadius);
		Attenuation.FalloffDistance = VoiceFalloffDistance;
		if (bVoiceOcclusion)
		{
			// Muffled, and quieter, through a wall.
			Attenuation.bEnableOcclusion = true;
			Attenuation.OcclusionTraceChannel = ECC_Visibility;
			Attenuation.OcclusionLowPassFilterFrequency = 1800.0f;
			Attenuation.OcclusionVolumeAttenuation = VoiceOcclusionVolume;
			Attenuation.OcclusionInterpolationTime = 0.4f;
		}
		Audio->SetSound(Wave);
		Audio->SetVolumeMultiplier(FMath::Max(VoiceVolume, 0.0001f));
		Audio->RegisterComponent();
		Audio->Play();
		VoiceAudio.Add(Audio);
	}

	Session = MakeShared<FPipecatSession>(this, ObjectPtrDecay(VoiceWaves), Functions, Transport, VoiceTracks);
	if (bUseMicrophone)
	{
		Session->StartMicrophone(bGateMicrophone, MicrophoneGateDelay, MicrophoneDevice);
	}

	if (bConnectOnBeginPlay)
	{
		Connect();
	}
}

void UPipecatVoiceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Disconnect();
	for (UAudioComponent* Audio : VoiceAudio)
	{
		if (Audio)
		{
			Audio->Stop();
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UPipecatVoiceComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// If something stops one of the bot's voices, play it again, without what
	// was waiting to be played.
	for (int32 Channel = 0; Channel < VoiceAudio.Num(); ++Channel)
	{
		UAudioComponent* Audio = VoiceAudio[Channel];
		if (Audio && IsReady() && !Audio->IsPlaying())
		{
			UE_LOG(LogPipecat, Warning, TEXT("The bot's voice %d stopped playing, playing it again"), Channel);
			VoiceWaves[Channel]->ResetAudio();
			Audio->Play();
		}
	}
}

void UPipecatVoiceComponent::Connect()
{
	if (!Session)
	{
		if (VoiceWaves.IsEmpty())
		{
			// Not playing yet: it connects when play begins, if it's to.
			bConnectOnBeginPlay = true;
			return;
		}
		Session = MakeShared<FPipecatSession>(this, ObjectPtrDecay(VoiceWaves), Functions, Transport, VoiceTracks);
		if (bUseMicrophone)
		{
			Session->StartMicrophone(bGateMicrophone, MicrophoneGateDelay, MicrophoneDevice);
		}
	}
	Session->Start(StartUrl, ApiKey);
}

void UPipecatVoiceComponent::Disconnect()
{
	if (Session)
	{
		Session->Stop();
		Session.Reset();
	}
}

void UPipecatVoiceComponent::SendText(const FString& Text)
{
	if (Session)
	{
		Session->SendText(Text);
	}
}

void UPipecatVoiceComponent::SendClientMessage(const FString& Type, const FString& DataJson)
{
	if (Session)
	{
		Session->SendClientMessage(Type, DataJson);
	}
}

bool UPipecatVoiceComponent::IsReady() const
{
	return Session && Session->IsReady();
}

float UPipecatVoiceComponent::GetBotVoiceLevel() const
{
	float Level = 0.0f;
	for (int32 Channel = 0; Channel < VoiceChannels; ++Channel)
	{
		Level = FMath::Max(Level, GetBotVoiceChannelLevel(Channel));
	}
	return Level;
}

float UPipecatVoiceComponent::GetBotVoiceChannelLevel(int32 Channel) const
{
	return Session ? Session->GetBotVoiceLevel(Channel) : 0.0f;
}

float UPipecatVoiceComponent::GetMicrophoneLevel() const
{
	return Session ? Session->GetMicrophoneLevel() : 0.0f;
}

void UPipecatVoiceComponent::SetMicrophoneOpen(bool bOpen)
{
	if (Session)
	{
		Session->SetMicrophoneOpen(bOpen);
	}
}

float UPipecatVoiceComponent::GetMicrophoneRms() const
{
	return Session ? Session->GetMicrophoneRms() : 0.0f;
}

void UPipecatVoiceComponent::GetMicrophoneWaveform(TArray<float>& OutSamples, int32 NumSamples) const
{
	if (Session)
	{
		Session->GetMicrophoneWaveform(OutSamples, NumSamples);
	}
	else
	{
		OutSamples.Init(0.0f, FMath::Max(NumSamples, 0));
	}
}

void UPipecatVoiceComponent::RegisterFunction(const FString& Name, FPipecatFunctionHandler Handler)
{
	Functions.Add(Name, MoveTemp(Handler));
}

void UPipecatVoiceComponent::AttachVoiceTo(USceneComponent* Parent, FName Socket)
{
	AttachVoiceChannelTo(0, Parent, Socket);
}

void UPipecatVoiceComponent::AttachVoiceChannelTo(int32 Channel, USceneComponent* Parent, FName Socket)
{
	if (!VoiceAudio.IsValidIndex(Channel) || !VoiceAudio[Channel])
	{
		return;
	}
	UAudioComponent* Audio = VoiceAudio[Channel];
	if (!Parent)
	{
		Parent = GetOwner()->GetRootComponent();
		Socket = NAME_None;
	}
	if (Parent && (Audio->GetAttachParent() != Parent || Audio->GetAttachSocketName() != Socket))
	{
		Audio->AttachToComponent(Parent, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
	}
}
