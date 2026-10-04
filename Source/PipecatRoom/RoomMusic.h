//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"

#include "RoomMusic.generated.h"

class UAudioComponent;
class USoundWaveProcedural;
class FRoomSynth;

// What a music source plays: a gentle tune on the piano, or something to dance
// to from the gramophone.
enum class ERoomMusicStyle : uint8
{
	Piano,
	Dance,
};

// The house's sounds, each made as it's needed.
enum class ERoomSound : uint8
{
	// The dinner bell.
	Bell,
	// Something appearing, or being picked.
	Pop,
	// A coin landing in the fountain.
	Plink,
	// Something given, or something good.
	Chime,
};

// Music from a place in the house, e.g. the piano or the gramophone, made as it
// plays by a little synthesizer: chords that go round, a bass, an arpeggio,
// and drums to dance to. It plays from where it's attached, carrying through
// the house and muffled through walls, and says where the beat is, for those
// dancing to it and for the lights.
UCLASS()
class URoomMusic : public USceneComponent
{
	GENERATED_BODY()

public:
	URoomMusic();
	virtual ~URoomMusic() override;

	void Play(ERoomMusicStyle Style);
	void Stop();
	bool IsPlaying() const { return bPlaying; }
	ERoomMusicStyle GetStyle() const { return Style; }

	/** Beats since it started, as it's heard (the fraction is how far into the beat). */
	double GetBeat() const;

	/** How much is sounding right now, from 0 to 1, e.g. for lights to pulse with. */
	float GetLoudness() const { return Loudness; }

	/** Quieter, by `Amount` from 0 to 1, e.g. while someone speaks over it. */
	void SetDuck(float Amount) { Duck = FMath::Clamp(Amount, 0.0f, 1.0f); }

	/** A few notes, as someone plays them, e.g. the player trying the piano. */
	void PlayNotes();

	/** How far it carries: at full volume within `Inner` cm, and fading out over `Falloff` more. */
	void SetReach(float Inner, float Falloff);

	/** A sound on its own, at a place, e.g. the bell ringing. */
	static void PlaySound(UWorld* World, const FVector& Where, ERoomSound Sound, float Volume = 1.0f);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void Ensure();
	int32 Queued() const;

	UPROPERTY()
	TObjectPtr<UAudioComponent> Audio;

	UPROPERTY()
	TObjectPtr<USoundWaveProcedural> Wave;

	// Owned: made when it first plays, and deleted with it.
	FRoomSynth* Synth = nullptr;
	ERoomMusicStyle Style = ERoomMusicStyle::Piano;
	bool bPlaying = false;
	float Inner = 300.0f;
	float Falloff = 2600.0f;
	float Duck = 0.0f;
	float Volume = 1.0f;
	float Loudness = 0.0f;
	TArray<int16> Buffer;
};
