//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomVoiceRing.h"

#include "Materials/MaterialInterface.h"

namespace
{
// Created by setup.ps1.
const TCHAR* const RingPath = TEXT("/Game/Room/M_Ring.M_Ring");

// How many points go around it, and how many ripples it shows at once.
const int32 Points = 192;
const int32 MaxRipples = 3;
// Its bands, each a ribbon: the waveform around the player's feet and its
// glow, the edge of the voice's reach (the waveform again, smaller) and its
// glow, and the ripples running out to it.
const int32 Bands = 4 + MaxRipples;
// How far from the player the waveform around their feet is, in cm.
const float Near = 75.0f;

const FLinearColor Core(0.72f, 0.9f, 1.0f);
const FLinearColor Glow(0.35f, 0.62f, 1.0f);

// A color, as visible as `Alpha`: the ring's material scales its glow by it.
FLinearColor Fade(const FLinearColor& Color, float Alpha)
{
	return FLinearColor(Color.R, Color.G, Color.B, FMath::Clamp(Alpha, 0.0f, 1.0f));
}

float Ease(float X)
{
	X = FMath::Clamp(X, 0.0f, 1.0f);
	return 1.0f - (1.0f - X) * (1.0f - X);
}
} // namespace

URoomVoiceRing::URoomVoiceRing(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
	SetVisibleInRayTracing(false);
	TranslucencySortPriority = 10;
}

void URoomVoiceRing::Build()
{
	bBuilt = true;
	RingMaterial = LoadObject<UMaterialInterface>(nullptr, RingPath);

	// Each band's inner and outer edge, point by point, and then the glow
	// inside it: its middle, and its edge.
	const int32 NumVertices = Bands * 2 * Points + Points + 1;
	Vertices.Init(FVector::ZeroVector, NumVertices);
	Colors.Init(FLinearColor::Transparent, NumVertices);
	TArray<FVector2D> UV;
	UV.Init(FVector2D::ZeroVector, NumVertices);
	TArray<FVector> Normals;
	Normals.Init(FVector::UpVector, NumVertices);
	TArray<int32> Triangles;
	for (int32 Band = 0; Band < Bands; ++Band)
	{
		const int32 Base = Band * 2 * Points;
		for (int32 I = 0; I < Points; ++I)
		{
			const int32 J = (I + 1) % Points;
			UV[Base + 2 * I] = FVector2D(0.0f, 0.0f);
			UV[Base + 2 * I + 1] = FVector2D(0.0f, 1.0f);
			Triangles.Append({Base + 2 * I, Base + 2 * J, Base + 2 * I + 1});
			Triangles.Append({Base + 2 * I + 1, Base + 2 * J, Base + 2 * J + 1});
		}
	}
	const int32 Disc = Bands * 2 * Points;
	for (int32 I = 0; I <= Points; ++I)
	{
		UV[Disc + I] = FVector2D(0.0f, 0.5f);
	}
	for (int32 I = 0; I < Points; ++I)
	{
		Triangles.Append({Disc + Points, Disc + I, Disc + (I + 1) % Points});
	}
	CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UV, Colors, {}, false);
	if (RingMaterial)
	{
		SetMaterial(0, RingMaterial);
	}
	Shape.Init(0.0f, Points);
}

void URoomVoiceRing::Draw(
	float DeltaSeconds, const FVector& Center, float Radius, float Presence, float Loudness, const TArray<float>& Waveform)
{
	if (!bBuilt)
	{
		Build();
	}
	SetVisibility(Presence > 0.002f);
	if (Presence <= 0.002f)
	{
		Ripples.Reset();
		return;
	}
	SetWorldLocation(Center);

	// The waveform, as loud as it is lately, around the ring, mirrored so it
	// meets itself.
	float Loudest = 0.0f;
	for (float Sample : Waveform)
	{
		Loudest = FMath::Max(Loudest, FMath::Abs(Sample));
	}
	Peak = FMath::Max(FMath::Max(Peak * FMath::Exp(-DeltaSeconds * 1.5f), Loudest), 0.02f);
	// Each point is the average of the samples around it, so the ring shows
	// the voice's shape rather than its hiss.
	const int32 Half = Points / 2;
	const int32 Samples = Waveform.Num();
	const int32 Window = FMath::Max(Samples / Half, 1);
	for (int32 I = 0; I < Points; ++I)
	{
		float Value = 0.0f;
		if (Samples > 1)
		{
			const float T = (I <= Half ? I : Points - I) / static_cast<float>(Half);
			const int32 Center = FMath::RoundToInt(T * (Samples - 1));
			float Sum = 0.0f;
			int32 Count = 0;
			for (int32 J = FMath::Max(Center - Window, 0); J <= FMath::Min(Center + Window, Samples - 1); ++J)
			{
				Sum += Waveform[J];
				++Count;
			}
			Value = FMath::Clamp(Sum / FMath::Max(Count, 1) / Peak, -1.0f, 1.0f);
		}
		Shape[I] = FMath::Lerp(Shape[I], Value, 0.5f);
	}

	// Ripples run out from the player to the edge while they speak.
	SinceRipple += DeltaSeconds;
	if (Presence > 0.6f && SinceRipple > 0.55f && Ripples.Num() < MaxRipples)
	{
		SinceRipple = 0.0f;
		Ripples.Add(0.0f);
	}
	for (float& Ripple : Ripples)
	{
		Ripple += DeltaSeconds / 1.1f;
	}
	Ripples.RemoveAll([](float Ripple) { return Ripple >= 1.0f; });

	const float Amplitude = (10.0f + 34.0f * Loudness) * Presence;
	auto Band = [&](int32 Index, auto RadiusAt, float HalfWidth, const FLinearColor& Color) {
		const int32 Base = Index * 2 * Points;
		for (int32 I = 0; I < Points; ++I)
		{
			const float Angle = UE_TWO_PI * I / Points;
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
			const float R = RadiusAt(I);
			Vertices[Base + 2 * I] = Direction * FMath::Max(R - HalfWidth, 0.0f);
			Vertices[Base + 2 * I + 1] = Direction * (R + HalfWidth);
			Colors[Base + 2 * I] = Color;
			Colors[Base + 2 * I + 1] = Color;
		}
	};
	// The voice itself, around their feet, and the edge of its reach, which
	// trembles with it.
	auto Feet = [&](int32 I) { return Near + Amplitude * Shape[I]; };
	auto Edge = [&](int32 I) { return Radius + 0.45f * Amplitude * Shape[I]; };
	Band(0, Feet, 2.5f, Fade(Core, Presence));
	Band(1, Feet, 14.0f, Fade(Glow, 0.35f * Presence));
	Band(2, Edge, 4.0f, Fade(Core, 0.9f * Presence));
	Band(3, Edge, 30.0f, Fade(Glow, 0.25f * Presence));
	for (int32 R = 0; R < MaxRipples; ++R)
	{
		const bool bShown = R < Ripples.Num();
		const float P = bShown ? Ripples[R] : 0.0f;
		const float At = FMath::Lerp(Near, Radius, Ease(P));
		const float Alpha = bShown ? 0.6f * (1.0f - P * P) * Presence : 0.0f;
		Band(4 + R, [At](int32) { return At; }, 3.0f, Fade(Glow, Alpha));
	}
	const int32 Disc = Bands * 2 * Points;
	for (int32 I = 0; I < Points; ++I)
	{
		const float Angle = UE_TWO_PI * I / Points;
		Vertices[Disc + I] = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * FMath::Max(Edge(I) - 4.0f, 0.0f);
		Colors[Disc + I] = Fade(Glow, 0.025f * Presence);
	}
	Vertices[Disc + Points] = FVector::ZeroVector;
	Colors[Disc + Points] = Fade(Glow, 0.0f);

	UpdateMeshSection_LinearColor(0, Vertices, {}, {}, Colors, {});
}
