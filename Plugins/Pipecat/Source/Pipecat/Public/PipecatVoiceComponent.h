//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "PipecatVoiceComponent.generated.h"

class UAudioComponent;
class USceneComponent;
class USoundWaveProcedural;

/** How the game talks to the bot. */
UENUM(BlueprintType)
enum class EPipecatTransport : uint8
{
	/** Over Daily (WebRTC): the bot's start endpoint creates a Daily room, e.g. `bot.py -t daily`. */
	Daily,
	/** Over a WebSocket, e.g. `bot.py -t websocket`. */
	WebSocket,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPipecatEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPipecatTextEvent, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPipecatTranscriptEvent, const FString&, Text, bool, bFinal);

// Reacts to the bot's LLM calling a function, on the game thread, with the
// call's arguments as JSON. The bot runs the function and gives the LLM its
// result.
using FPipecatFunctionHandler = TFunction<void(const FString& ArgumentsJson)>;

/**
 * Talks to a Pipecat bot with voice: starts the bot, sends it the microphone,
 * and plays its voice from the owner's position in the world.
 *
 * A bot with several voices, e.g. one per character, can send each on a
 * channel of its own (VoiceChannels), and each channel plays from where it's
 * attached, e.g. its character's head, so they can speak at once, each from
 * where they are.
 *
 * Events run on the game thread.
 */
UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent))
class PIPECAT_API UPipecatVoiceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPipecatVoiceComponent();

	/** The bot's start endpoint, e.g. http://localhost:7860/start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	FString StartUrl = TEXT("http://localhost:7860/start");

	/** Sent as a bearer token to the start endpoint, e.g. a Pipecat Cloud public API key. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	FString ApiKey;

	/** How the game talks to the bot. Set it before connecting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	EPipecatTransport Transport = EPipecatTransport::Daily;

	/**
	 * Over Daily, the bot's custom audio tracks its voices come from, in the
	 * order of the voice channels, e.g. a track per character (a transport
	 * destination each, in the bot). Empty: its one voice, from its
	 * microphone. Over a WebSocket, the bot sends its voices as the channels
	 * of one stream instead. Set it before play begins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	TArray<FString> VoiceTracks;

	/** Starts the bot and connects when play begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	bool bConnectOnBeginPlay = true;

	/** Sends the microphone to the bot. It's captured from when play begins, connected or not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	bool bUseMicrophone = true;

	/**
	 * Sends the microphone only while the game says the player is speaking
	 * (SetMicrophoneOpen), and silence otherwise, so that what else it picks
	 * up (the game's own music and voices, say, without headphones) isn't
	 * taken for the player. It's sent MicrophoneGateDelay seconds late, so the
	 * start of what they say, before the game has noticed, isn't lost. Set it
	 * before play begins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	bool bGateMicrophone = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat", meta = (ClampMin = "0", ClampMax = "1"))
	float MicrophoneGateDelay = 0.2f;

	/** How many voices the bot sends, each on a channel of its own. Set it before play begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat", meta = (ClampMin = "1", ClampMax = "8"))
	int32 VoiceChannels = 1;

	/** How far away a voice is heard at full volume, in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	float VoiceInnerRadius = 300.0f;

	/** How far beyond that a voice fades out over, in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	float VoiceFalloffDistance = 4000.0f;

	/** How loud the bot's voices play, from 0 (silent, still playing) to 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat", meta = (ClampMin = "0", ClampMax = "4"))
	float VoiceVolume = 1.0f;

	/** Whether a voice is muffled, and quieter, behind something that blocks the view of it, e.g. a wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat")
	bool bVoiceOcclusion = false;

	/** How loud a voice is behind something, from 0 to 1, with occlusion on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipecat", meta = (ClampMin = "0", ClampMax = "1"))
	float VoiceOcclusionVolume = 0.45f;

	/** Starts the bot and connects to it, without waiting. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void Connect();

	/** Disconnects from the bot. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void Disconnect();

	/** Sends text to the bot, as if the user said it. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void SendText(const FString& Text);

	/** Sends a message of the game's own to the bot, with its data as JSON, e.g. {"heard": ["maya"]}. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void SendClientMessage(const FString& Type, const FString& DataJson);

	/** Whether the bot is connected and ready. */
	UFUNCTION(BlueprintPure, Category = "Pipecat")
	bool IsReady() const;

	/** How loud the bot's voice is right now, from 0 to 1: the loudest of its channels. */
	UFUNCTION(BlueprintPure, Category = "Pipecat")
	float GetBotVoiceLevel() const;

	/** How loud one of the bot's voices is right now, from 0 to 1. */
	UFUNCTION(BlueprintPure, Category = "Pipecat")
	float GetBotVoiceChannelLevel(int32 Channel) const;

	/** How loud the microphone is right now, from 0 to 1. */
	UFUNCTION(BlueprintPure, Category = "Pipecat")
	float GetMicrophoneLevel() const;

	/** With bGateMicrophone, whether the player is speaking, and so the microphone is sent. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void SetMicrophoneOpen(bool bOpen);

	/** How loud the microphone is right now, as the RMS of its last 10 ms, from 0 to 1 (full scale). */
	UFUNCTION(BlueprintPure, Category = "Pipecat")
	float GetMicrophoneRms() const;

	/** The microphone's last `NumSamples` samples (at 16 kHz), oldest first, from -1 to 1. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void GetMicrophoneWaveform(TArray<float>& OutSamples, int32 NumSamples = 512) const;

	/** Reacts to calls to one of the bot's functions. Register it before connecting. */
	void RegisterFunction(const FString& Name, FPipecatFunctionHandler Handler);

	/**
	 * Plays the bot's voice from `Parent`, e.g. a character's head, or from the
	 * owner again if it's null. With several channels, the first.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void AttachVoiceTo(USceneComponent* Parent, FName Socket = NAME_None);

	/** Plays one of the bot's voices from `Parent`, or from the owner again if it's null. */
	UFUNCTION(BlueprintCallable, Category = "Pipecat")
	void AttachVoiceChannelTo(int32 Channel, USceneComponent* Parent, FName Socket = NAME_None);

	/** The bot is ready to talk. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnBotReady;

	/** The connection ended. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnDisconnected;

	/** Something went wrong. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatTextEvent OnError;

	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnUserStartedSpeaking;

	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnUserStoppedSpeaking;

	/** What the user said. Partial transcripts come before the final one. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatTranscriptEvent OnUserTranscript;

	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnBotStartedSpeaking;

	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnBotStoppedSpeaking;

	/** The user interrupted the bot. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatEvent OnBotInterrupted;

	/** Text the bot says, a sentence at a time, as it starts saying it. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatTextEvent OnBotOutput;

	/** A message of the bot's own, as JSON, e.g. {"type": "speaker", "speaker": "orb"}. */
	UPROPERTY(BlueprintAssignable, Category = "Pipecat")
	FPipecatTextEvent OnServerMessage;

	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// Each of the bot's voices, played from where it's attached.
	UPROPERTY()
	TArray<TObjectPtr<UAudioComponent>> VoiceAudio;

	UPROPERTY()
	TArray<TObjectPtr<USoundWaveProcedural>> VoiceWaves;

	TSharedPtr<class FPipecatSession> Session;
	TMap<FString, FPipecatFunctionHandler> Functions;
};
