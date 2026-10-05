//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoomThings.h"
#include "RoomTypes.h"

#include "RoomStage.generated.h"

class APawn;
class ARoomCharacter;
class ARoomHouse;
class ARoomItem;
class FJsonObject;
class USkeletalMeshComponent;
class SRoomBubbles;
class SRoomCaptions;
class UPipecatVoiceComponent;
class URoomVoiceRing;

// The conversation in the house: it starts the bot and connects to it, puts
// each character's voice in their head, and tells the bot who hears what.
//
// The player's voice carries as far as they speak up: a ring on the floor
// around them shows how far, as they speak, and only the characters inside it
// hear them (half as far through a wall). The bot is told who heard each thing
// the player said, who can hear each character, where everyone is, and when
// the player first comes up to someone. It tells the game who's speaking and
// what they say, how they feel and what they do, and where they go.
UCLASS(Config = Game)
class ARoomStage : public AActor
{
	GENERATED_BODY()

public:
	ARoomStage();

	/** The characters, as the bot has them, relative to the project's folder. */
	UPROPERTY(Config)
	FString CastFile = TEXT("bot/characters.json");

	/**
	 * How the game talks to the bot: "daily" (`bot.py -t daily`, each
	 * character's voice on a Daily track of its own) or "websocket"
	 * (`bot.py -t websocket`, their voices as the channels of one stream).
	 * -PipecatTransport= on the command line wins.
	 */
	UPROPERTY(Config)
	FString Transport = TEXT("daily");

	/** How far the player's voice carries at their usual loudness, in cm, and at most and at least. */
	UPROPERTY(Config)
	float NormalRange = 600.0f;

	UPROPERTY(Config)
	float MinRange = 280.0f;

	UPROPERTY(Config)
	float MaxRange = 2200.0f;

	/** How far a voice carries through a wall, compared to in the open. */
	UPROPERTY(Config)
	float ThroughWalls = 0.5f;

	/** How far a character's voice carries, in cm: who hears them, the player included. */
	UPROPERTY(Config)
	float CharacterRange = 650.0f;

	/**
	 * How the characters' voices sound with distance: at full volume within
	 * so many cm, fading out over so many more, and how loud through a wall,
	 * from 0 to 1.
	 */
	UPROPERTY(Config)
	float VoiceFullWithin = 150.0f;

	UPROPERTY(Config)
	float VoiceFadesOver = 850.0f;

	UPROPERTY(Config)
	float VoiceThroughWalls = 0.3f;

	/** Within this many cm, in plain view, the player comes up to a character, who greets them. */
	UPROPERTY(Config)
	float MeetDistance = 480.0f;

	/**
	 * What the player's looking at is labeled on the screen within this many
	 * cm of them (to its nearest side). The bot's told about it from further.
	 */
	UPROPERTY(Config)
	float LabelDistance = 450.0f;

	/**
	 * The quietest the microphone counts as the player speaking, in dBFS,
	 * however quiet the room: lower for a quiet microphone, higher for a noisy
	 * room.
	 */
	UPROPERTY(Config)
	float SpeechMinDb = -46.0f;

	/**
	 * While a character's voice is playing, sends the bot the microphone only
	 * while the player's speaking over it, and silence otherwise, so the
	 * character's voice, if the microphone picks it up, isn't taken for the
	 * player. The rest of the time, the bot's own VAD picks out their speech
	 * (bot/speech.py), and this game's reading of their voice is only what
	 * they see of it.
	 */
	UPROPERTY(Config)
	bool bGateMicrophone = true;

	/**
	 * The microphone to use: part of its name, e.g. "Yeti". Empty: Windows'
	 * default, unless that's a game controller's, when it's the first that
	 * isn't. -PipecatMicrophone= on the command line wins.
	 */
	UPROPERTY(Config)
	FString MicrophoneDevice;

	/** The bot's start endpoint, and its API key, if it needs one. Set before play begins. */
	FString StartUrl;
	FString ApiKey;
	bool bUseMicrophone = true;
	/** How loud the characters' voices play, from 0 (silent) to 1. */
	float VoiceVolume = 1.0f;
	/** Typed to the bot once it's ready, e.g. to try it without a microphone. */
	FString FirstMessage;
	bool bShowStatus = false;

	virtual void Tick(float DeltaSeconds) override;

	/** For trying it out: as if the player said this, at their usual loudness. */
	void Say(const FString& Text);
	/** For trying it out: as if the player spoke for so many seconds, as loud as `Loudness` (in dB above usual). */
	void Pretend(float Seconds, float Loudness);
	/** A message as if from the bot, e.g. {"type": "emote", "who": "maya", "mood": "happy"}. */
	void HandleMessage(const FString& Message);

	/** Whatever the player can do where they are, as they press E. */
	void Interact();
	/** Has a character do something, as the bot asks, e.g. "dance", "play" (the piano) or "food". */
	/** Has a character do something: for the player, or for `For`, another character, e.g. bring them cake. */
	void Act(ARoomCharacter* Character, const FString& Action, ARoomCharacter* For = nullptr, const FString& Color = FString());
	/** For trying it out: has a character go about their day now, or visit another. */
	void Routine(ARoomCharacter* Character);
	void Visit(ARoomCharacter* Visitor, ARoomCharacter* Host);

	ARoomCharacter* FindCharacter(const FString& Id) const;
	const TArray<TObjectPtr<ARoomCharacter>>& GetCharacters() const { return Characters; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool LoadCast();
	void SpawnCharacters();
	void ConnectVoice();
	void AddCaptions();

	// Each frame: how the player is speaking, who hears them, the ring, and
	// where everyone is.
	void UpdateSpeech(float DeltaSeconds);
	void UpdateEarshot(float DeltaSeconds);
	void UpdateCharacters(float DeltaSeconds);
	void UpdateSpace(float DeltaSeconds);
	// Where each bubble hangs, over its speaker's head, and how they're seen and heard.
	void UpdateBubbles(float DeltaSeconds);
	// What the player's looking at: whatever's nearest the middle of their view, in plain
	// sight, for the bot (in `space`) and the tag on the screen.
	void UpdateLook(float DeltaSeconds);

	// Whether a voice from `From` reaches `To`, carrying `Range` in the open.
	bool Reaches(const FVector& From, const FVector& To, float Range, const AActor* Ignore, const AActor* Ignore2) const;
	FVector PlayerHead() const;
	AActor* Resolve(const FString& Id) const;
	float Clarity(const ARoomCharacter* Character) const;
	FString Label(const ARoomCharacter* Character) const;
	void Learn(const FString& Text);
	void Send(const FString& Type, const TSharedRef<FJsonObject>& Data);
	void SendWorld();
	void Move(const TArray<ARoomCharacter*>& Who, const FString& Action, FName Area);

	// The house's life: what the player can do where they are, the music and
	// who's dancing to it, and the characters going about their day.
	void BindInput();
	void UpdatePrompt(float DeltaSeconds);
	void UpdateLife(float DeltaSeconds);
	// Something that happened, for the bot, and who saw or heard it.
	void Event(const FString& Kind, const TSharedRef<FJsonObject>& Data, const TArray<FString>& HeardBy);
	TArray<FString> Witnesses(const FVector& Where, float Carry) const;
	// A character hands what they're holding to someone.
	void Give(ARoomCharacter* Giver, AActor* To);
	// A character eats what they're holding, if it's food and they're free.
	void EatHeld(ARoomCharacter* Character);

	// Someone taking the player to meet someone else: once they're all within
	// earshot of each other, they introduce them (the `introduce` event).
	struct FIntroduction
	{
		TWeakObjectPtr<ARoomCharacter> Host;
		TWeakObjectPtr<ARoomCharacter> Guest;
		double Since = 0.0;
		// When the host last set off after the guest, and where the guest was then.
		double Walked = 0.0;
		FVector GuestAt = FVector::ZeroVector;
	};
	TArray<FIntroduction> Introductions;
	void UpdateIntroductions();
	// Whether someone's waiting to be introduced to the player: they stay put.
	bool IsBeingIntroduced(const ARoomCharacter* Character) const;
	// The host sets off to where the guest is now.
	void WalkToGuest(FIntroduction& Introduction);
	void PlayerHolds(ARoomItem* Item);
	USkeletalMeshComponent* PlayerHand() const;
	FString Called(const ARoomCharacter* Character) const;
	ARoomCharacter* Owner(FName Area) const;

	UFUNCTION()
	void HandleServerMessage(const FString& Message);
	UFUNCTION()
	void HandleUserTranscript(const FString& Text, bool bFinal);
	UFUNCTION()
	void HandleBotReady();
	UFUNCTION()
	void HandleDisconnected();
	UFUNCTION()
	void HandleError(const FString& Error);

	UPROPERTY()
	TObjectPtr<UPipecatVoiceComponent> Voice;

	UPROPERTY()
	TObjectPtr<URoomVoiceRing> Ring;

	UPROPERTY()
	TArray<TObjectPtr<ARoomCharacter>> Characters;

	UPROPERTY()
	TObjectPtr<ARoomThings> Things;

	// What the player is holding, if anything.
	UPROPERTY()
	TObjectPtr<ARoomItem> PlayerItem;

	// What the player can do now, with E: something with a thing, give what
	// they're holding to someone, or eat it, or put it down.
	enum class EAction : uint8
	{
		None,
		Thing,
		GiveTo,
		Eat,
		PutDown,
	};
	EAction Action = EAction::None;
	ARoomThings::FInteraction Interaction;
	TWeakObjectPtr<ARoomCharacter> GiveTarget;
	bool bInputBound = false;
	// When each character next goes about their day, or visits someone.
	TMap<FString, double> NextRoutine;
	TMap<FString, double> NextVisit;
	// When each character last heard the player or spoke, so they don't wander
	// off in the middle of a conversation.
	TMap<FString, double> Engaged;
	double Clock = 0.0;

	TWeakObjectPtr<ARoomHouse> House;
	TArray<FRoomCast> Members;
	TSharedPtr<SRoomCaptions> Captions;
	TSharedPtr<SRoomBubbles> Bubbles;
	// Where each character's bubble hangs from, eased as they move.
	TMap<FString, FVector> BubbleAnchors;

	// What the player's looking at, as the bot's told: a character's id, or what a thing's
	// called, e.g. "the flowers"; its tag, and where; and what might be next, if it holds a
	// moment. And the last thing they looked at before, and when, for a question asked as
	// they turn to someone ("what are those?").
	FString Looking;
	FString LookingLabel;
	FVector LookingAt = FVector::ZeroVector;
	float LookingRadius = 0.0f;
	TWeakObjectPtr<ARoomCharacter> LookingAtCharacter;
	FString NextLook;
	float NextLookFor = 0.0f;
	// Looking at someone, the thing in view with them, e.g. the flowers they're standing by.
	FString LookingThing;
	FString NextThing;
	float NextThingFor = 0.0f;
	FString LookedAt;
	double LookedAtWhen = -1000.0;

	// The player's voice: how loud it is, in dB, and how loud usually; how
	// loud the room is without it; whether they're speaking, and how far it
	// carries.
	float Level = -90.0f;
	float Peak = -90.0f;
	float Usual = -32.0f;
	float Floor = -60.0f;
	float Smoothed = -60.0f;
	float Onset = 0.0f;
	bool bSpeaking = false;
	float Quiet = 10.0f;
	float Hangover = 0.0f;
	float Range = 0.0f;
	float Presence = 0.0f;
	// How long since a character's voice was last heard playing.
	float SinceVoices = 10.0f;
	float PretendLeft = 0.0f;
	float PretendLoudness = 0.0f;
	TArray<float> Waveform;

	// Who heard what the player is saying, since they started.
	TSet<FString> Heard;
	FString LastEarshot;
	TSet<FString> Met;
	FString LastSpace;
	float SpaceIn = 0.0f;

	// Whose names the player has heard, and who's speaking, by the bot.
	TSet<FString> Known;
	TSet<FString> Speaking;

	// Each character's line as it's said: shown from when their voice is heard,
	// a word at a time, and all of it once their voice has finished.
	struct FSaying
	{
		FString Id;
		FString Text;
		// About how many of its characters have been said, and how many are shown.
		float Said = 0.0f;
		int32 Shown = 0;
		float Clarity = -1.0f;
		float SinceVoice = 10.0f;
		bool bHeard = false;
		// The bot says their voice has stopped; and so it's all been said.
		bool bEnding = false;
		bool bWhole = false;
	};
	TMap<FString, FSaying> Sayings;
	// Who's been asked to stop dancing: they sit this record out.
	TSet<FString> SittingOut;
	bool bWasGramophoneOn = false;
	// Who's being visited (by id), and so stays put for it.
	TSet<FString> BeingVisited;
	void UpdateSaying(ARoomCharacter* Character, FSaying& Saying, float ChannelLevel, float DeltaSeconds);

public:
	/** A character's caption, all of it, now, e.g. for Room.Caption. */
	void ShowWhole(const FString& Speaker);

private:
	TMap<FString, FString> Lines;
	// When each character was last logged speaking, by their voice.
	TMap<FString, double> LoggedVoice;
	bool bReady = false;
};
