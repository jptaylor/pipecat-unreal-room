//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoomHouse.h"
#include "RoomTypes.h"

#include "RoomThings.generated.h"

class URoomMusic;
class UStaticMeshComponent;

// The things in the house that the characters, and the player, do things
// with: the gramophone in the hall, which plays music to dance to, and the
// piano in the music room; the stove in the kitchen, where cakes are baked,
// the cake stand on the island and the dinner bell; the flowers and the
// tomatoes in the conservatory, its trees, which grow when they're watered,
// and its fountain, for wishes; and the gallery's sculptures, which spin.
UCLASS()
class ARoomThings : public AActor
{
	GENERATED_BODY()

public:
	ARoomThings();

	/** Builds them in the house. Call once it's built. */
	void Build(ARoomHouse* InHouse);

	// Where a character stands to do things, and which way they face.
	FRoomSpot GetPianoSpot() const { return {FVector(-226.0f, 1225.0f, 0.0f), 180.0f}; }
	FRoomSpot GetStoveSpot() const { return {FVector(325.0f, -1866.0f, 0.0f), -90.0f}; }
	FRoomSpot GetCakeSpot() const { return {FVector(0.0f, -1385.0f, 0.0f), -90.0f}; }
	FRoomSpot GetGramophoneSpot() const { return {FVector(300.0f, 610.0f, 0.0f), 90.0f}; }
	FRoomSpot GetFlowerSpot() const { return {FVector(1600.0f, -740.0f, 0.0f), -90.0f}; }
	FRoomSpot GetTomatoSpot() const { return {FVector(1600.0f, 740.0f, 0.0f), 90.0f}; }
	FRoomSpot GetPlanterSpot(int32 Index) const;
	int32 GetPlanterCount() const { return 4; }

	// The music.
	void SetGramophone(bool bOn);
	bool IsGramophoneOn() const;
	void SetPiano(bool bPlaying);
	bool IsPianoPlaying() const;
	/** A few notes on the piano, as the player tries it. */
	void PlinkPiano();
	/** Where music is playing, if anywhere: the hall, or the music room. */
	FName GetMusicArea() const;
	/**
	 * The beat where someone stands, from the music they hear best there: beats
	 * since it started (the fraction is how far into the beat), and how well
	 * they hear it, from 0 (not at all) to 1.
	 */
	double GetBeatAt(const FVector& Where, float& OutStrength) const;
	/** How loud the music is, from 0 to 1, e.g. for the lights. */
	float GetMusicLoudness() const;
	/** Quieter while someone speaks over it, by `Amount` from 0 to 1. */
	void Duck(float Amount);

	// The kitchen.
	void SetCooking(bool bCooking);
	void BakeCake();
	bool HasCake() const { return bCake; }
	void RingBell();

	// The conservatory, and the gallery.
	void Water(int32 Planter);
	void Wish();
	void Spin(int32 Sculpture);

	/** Something the player can do where they are, e.g. "Put a record on", and what with. */
	struct FInteraction
	{
		FString Label;
		FName Thing;
		int32 Index = 0;
	};
	bool FindInteraction(const FVector& Where, const FVector& Facing, ERoomItem Held, FInteraction& Out) const;

	/** Where a thing is, e.g. "gramophone", for who hears or sees what's done with it. */
	FVector GetLocation(FName Thing, int32 Index = 0) const;

	virtual void Tick(float DeltaSeconds) override;

private:
	void BuildGramophone();
	void BuildKitchen();
	void BuildConservatory();

	TWeakObjectPtr<ARoomHouse> House;

	UPROPERTY()
	TObjectPtr<URoomMusic> Gramophone;

	UPROPERTY()
	TObjectPtr<URoomMusic> Piano;

	UPROPERTY()
	TObjectPtr<USceneComponent> Turntable;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Steam;

	UPROPERTY()
	TObjectPtr<USceneComponent> Cake;

	bool bCooking = false;
	bool bCake = false;
	float CakePresence = 0.0f;
	float Time = 0.0f;
	// How fast each sculpture spins, in degrees a second.
	TArray<float> Spins;
};
