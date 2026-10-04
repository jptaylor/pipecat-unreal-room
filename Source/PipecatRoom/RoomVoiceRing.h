//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

#include "RoomVoiceRing.generated.h"

// The reach of the player's voice, drawn on the floor around them as they
// speak: a ring as wide as their voice carries, whose edge is the waveform of
// what they're saying, with ripples running out to it, and a faint glow
// inside it.
UCLASS()
class URoomVoiceRing : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	URoomVoiceRing(const FObjectInitializer& ObjectInitializer);

	/**
	 * Redraws it around `Center`, on the floor: `Radius` across, as visible as
	 * `Presence` (0 to 1), with the waveform's samples (-1 to 1) around its
	 * edge, as big as `Loudness` (0 to 1).
	 */
	void Draw(float DeltaSeconds, const FVector& Center, float Radius, float Presence, float Loudness, const TArray<float>& Waveform);

private:
	void Build();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RingMaterial;

	TArray<FVector> Vertices;
	TArray<FLinearColor> Colors;
	// The waveform around the ring, eased from frame to frame.
	TArray<float> Shape;
	float Peak = 0.05f;
	// Ripples running out to the edge: how far each is, from 0 to 1.
	TArray<float> Ripples;
	float SinceRipple = 0.0f;
	bool bBuilt = false;
};
