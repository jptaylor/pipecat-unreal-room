//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RoomTypes.h"

#include "RoomCharacter.generated.h"

class ARoomHouse;
class ARoomItem;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class URoomCharacterAnim;

// What a character is doing about where they are: staying at home, waiting
// where they were told to, following the player, coming over to them, or
// going somewhere.
enum class ERoomIntent : uint8
{
	Home,
	Wait,
	Follow,
	Come,
	Go,
};

// A step of something a character's been asked to do: walk somewhere and face
// a way, be busy at something there for a while (or until they're stopped),
// take something in their hand, give what they're holding to someone, make a
// gesture, or have something happen, e.g. a cake being baked.
struct FRoomStep
{
	enum class EKind : uint8
	{
		Walk,
		Busy,
		Take,
		Give,
		Gesture,
		Call,
	};

	EKind Kind = EKind::Walk;
	FVector Where = FVector::ZeroVector;
	float Yaw = 0.0f;
	ERoomActivity Activity = ERoomActivity::None;
	// How long they're busy, in seconds: 0 or less, until they're stopped.
	float Seconds = 0.0f;
	ERoomItem Item = ERoomItem::None;
	FLinearColor Tint = FLinearColor::Red;
	FString ItemName;
	TWeakObjectPtr<AActor> To;
	ERoomGesture Gesture = ERoomGesture::None;
	TFunction<void()> Then;

	static FRoomStep WalkTo(const FVector& Where, float Yaw);
	static FRoomStep BusyWith(ERoomActivity Activity, float Seconds);
	static FRoomStep TakeItem(ERoomItem Item, const FLinearColor& Tint = FLinearColor::Red, const FString& Name = FString());
	static FRoomStep GiveTo(AActor* To);
	static FRoomStep Make(ERoomGesture Gesture);
	static FRoomStep Call(TFunction<void()> Then);
};

// One of the characters: the template's mannequin, in their own color, who
// walks around the house, follows the player or waits, looks at whoever's
// talking, and poses and gestures with how they feel. Their voice comes from
// their head.
UCLASS()
class ARoomCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ARoomCharacter();

	/** Who they are and where they live. Call before they're finished spawning. */
	void Setup(const FRoomCast& InCast, ARoomHouse* InHouse);

	const FRoomCast& GetCast() const { return Info; }
	const FString& GetId() const { return Info.Id; }

	/** Where their voice comes from. */
	USceneComponent* GetVoiceParent() const;
	FName GetVoiceSocket() const { return TEXT("head"); }
	FVector GetHeadLocation() const;

	/** What they're asked to do: e.g. follow the player, or go to an area (Go). */
	void SetIntent(ERoomIntent InIntent, FName Area = NAME_None);
	/**
	 * They've talked with the player: from now on they stay where they are
	 * (Wait), rather than back to their place and their day, until they're
	 * sent back to it. What they're doing now, they finish.
	 */
	void Settle();
	ERoomIntent GetIntent() const { return Intent; }
	const TCHAR* GetIntentName() const;

	/** Where they stand when they come over, among others: one of `Count` places in front of the player. */
	void SetSlot(int32 InSlot, int32 InCount);

	/** How they feel, how much, for how long, in seconds. */
	void SetMood(ERoomMood Mood, float Strength, float Seconds);
	void Gesture(ERoomGesture Gesture, float Strength = 1.0f);

	/** Who they're looking at, e.g. whoever's talking, or who they're talking to. Null: whoever's near. */
	void LookAt(AActor* Target, float Seconds);
	/** Who they're talking to, if anyone, so they face them and point at them. */
	void TalkTo(AActor* Target);

	/**
	 * Something to do, step by step, instead of whatever they were doing, e.g.
	 * walk to the piano and play it, described for the bot, e.g. "playing the
	 * piano". `OnEnd` runs when it's done, or they're stopped. Then they go
	 * back to what they were doing before: following the player, say.
	 */
	void Do(TArray<FRoomStep> InSteps, const FString& InDoing, TFunction<void()> InOnEnd = nullptr);
	void StopDoing();
	bool IsBusy() const { return !Steps.IsEmpty(); }
	const FString& GetDoing() const { return Doing; }

	/** Dancing, to whatever music they hear; `bAuto`: because they hear it, so they stop when it stops. */
	void SetDancing(bool bOn, bool bAuto = false);
	bool IsDancing() const { return bDancing; }
	bool IsDancingToMusic() const { return bDancing && bAutoDance; }
	/** The beat of the music they hear best, and how well they hear it, from 0 to 1. */
	void SetBeat(double InBeat, float Strength);

	/** What they're holding in their right hand, if anything. */
	ARoomItem* GetHeld() const { return Held; }
	ERoomItem GetHeldKind() const;
	void Hold(ARoomItem* Item);
	/** Lets go of what they're holding, and returns it. */
	ARoomItem* Release();
	/** Called when they hand what they're holding to someone (the player, or another character). */
	TFunction<void(ARoomCharacter* Giver, AActor* To)> OnGive;

	/** How loud their voice is right now, from 0 to 1. */
	void SetVoiceLevel(float Level);
	/** Whether their line is being written: they look like they're about to speak. */
	void SetThinking(bool bInThinking) { bThinking = bInThinking; }
	/** Whether they can hear the player right now, as the player speaks. */
	void SetHearsPlayer(bool bHears);
	/** Whether they're listening to someone: the player, or another character. */
	void SetListening(AActor* Speaker);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	URoomCharacterAnim* GetAnim() const;
	void UpdateMovement(float DeltaSeconds);
	void UpdateFacing(float DeltaSeconds);
	void UpdateLook(float DeltaSeconds);
	void UpdatePose(float DeltaSeconds);
	void UpdateGlow(float DeltaSeconds);
	void MoveTo(const FVector& Goal, float Accept);
	void Stop();
	void UpdateJob(float DeltaSeconds);
	void NextStep();
	FVector SlotNearPlayer(bool bBehind) const;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> Material;

	UPROPERTY()
	TObjectPtr<UPointLightComponent> Aura;

	UPROPERTY()
	TObjectPtr<ARoomItem> Held;

	// What they're doing, step by step, and what to do when it ends.
	TArray<FRoomStep> Steps;
	FString Doing;
	TFunction<void()> OnEnd;
	float StepTime = 0.0f;
	bool bStepStarted = false;
	bool bFacingFixed = false;
	ERoomActivity Activity = ERoomActivity::None;
	float ActivityAmount = 0.0f;

	bool bDancing = false;
	bool bAutoDance = false;
	float DanceAmount = 0.0f;
	double Beat = 0.0;
	float BeatStrength = 0.0f;
	// Their own beat, dancing to no music at all.
	double OwnBeat = 0.0;
	float HoldAmount = 0.0f;

	TWeakObjectPtr<ARoomHouse> House;
	FRoomCast Info;

	ERoomIntent Intent = ERoomIntent::Home;
	FName GoArea;
	int32 Slot = 0;
	int32 SlotCount = 1;
	// Where they're walking to, the way there, and how close is close enough.
	TArray<FVector> Path;
	int32 PathIndex = 0;
	FVector Goal = FVector::ZeroVector;
	float Accept = 60.0f;
	bool bWalking = false;
	float Stuck = 0.0f;
	// Where whoever they're handing something to was, when they set off to them.
	FVector GiveFrom = FVector::ZeroVector;
	// How many times running they've been stuck where they are, and where.
	int32 StuckTimes = 0;
	FVector StuckAt = FVector::ZeroVector;
	float Replan = 0.0f;
	// Where they face when they've nowhere to be, and whether they're turning.
	float RestYaw = 0.0f;
	bool bTurning = false;

	// How they feel, eased, and for how much longer.
	float Mood[static_cast<int32>(ERoomMood::Count)] = {};
	ERoomMood TargetMood = ERoomMood::Neutral;
	float MoodStrength = 0.0f;
	float MoodLeft = 0.0f;
	ERoomGesture CurrentGesture = ERoomGesture::None;
	float GestureTime = 0.0f;
	float GestureStrength = 1.0f;

	TWeakObjectPtr<AActor> LookTarget;
	float LookLeft = 0.0f;
	TWeakObjectPtr<AActor> Addressee;
	TWeakObjectPtr<AActor> Speaker;
	// Which of the locomotion blend space's axes is speed, and which is the
	// direction they move in, relative to where they face (None if it has none).
	int32 SpeedAxis = 0;
	int32 DirectionAxis = INDEX_NONE;
	float LookYaw = 0.0f;
	float LookPitch = 0.0f;
	// With nobody to look at, they look around now and then.
	float GlanceYaw = 0.0f;
	float GlancePitch = 0.0f;
	float GlanceIn = 0.0f;

	float Voice = 0.0f;
	float Talk = 0.0f;
	float TalkHold = 0.0f;
	float Listen = 0.0f;
	bool bThinking = false;
	bool bHearsPlayer = false;
	float Attention = 0.0f;
	float Hearing = 0.0f;
	float Time = 0.0f;
	float Seed = 0.0f;
};
