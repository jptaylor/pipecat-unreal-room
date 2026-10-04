//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomThings.h"

#include "RoomMusic.h"
#include "RoomShapes.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"

using RoomShapes::EShape;
using RoomShapes::Srgb;

DEFINE_LOG_CATEGORY_STATIC(LogRoomThings, Log, All);

namespace
{
// Where things are.
const FVector GramophoneAt(300.0f, 715.0f, 0.0f);
const FVector KeyboardAt(-274.0f, 1225.0f, 95.0f);
const FVector PianoAt(-370.0f, 1225.0f, 100.0f);
const FVector PotAt(325.0f, -1928.0f, 97.0f);
const FVector CakeAt(0.0f, -1500.0f, 95.0f);
const FVector BellAt(255.0f, -905.0f, 0.0f);
const FVector FlowersAt(1600.0f, -820.0f, 0.0f);
const FVector TomatoesAt(1600.0f, 820.0f, 0.0f);
const FVector FountainAt(1700.0f, 0.0f, 0.0f);

// How close the player comes to do something with each.
const float Reach = 170.0f;

const FLinearColor Brass = Srgb(214, 168, 72);
const FLinearColor Wood = Srgb(130, 90, 64);
const FLinearColor Soil = Srgb(84, 62, 48);
const FLinearColor Leaf = Srgb(84, 140, 76);

} // namespace

ARoomThings::ARoomThings()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ARoomThings::Build(ARoomHouse* InHouse)
{
	House = InHouse;
	BuildGramophone();
	BuildKitchen();
	BuildConservatory();

	// The piano's music comes from inside it.
	Piano = NewObject<URoomMusic>(this, TEXT("Piano"));
	Piano->SetupAttachment(RootComponent);
	Piano->SetRelativeLocation(PianoAt);
	Piano->RegisterComponent();
	Piano->SetReach(300.0f, 2200.0f);

	Spins.Init(0.0f, House.IsValid() ? House->GetSculptureCount() : 0);
}

void ARoomThings::BuildGramophone()
{
	// A little round table by the wall, and on it a gramophone: a box, a
	// turntable with a record on it, and a brass horn.
	RoomShapes::Add(this, nullptr, EShape::Cylinder, GramophoneAt + FVector(0.0f, 0.0f, 36.0f), FVector(60.0f, 60.0f, 72.0f), Wood, 0.5f,
		FRotator::ZeroRotator, true);
	RoomShapes::Add(this, nullptr, EShape::Cube, GramophoneAt + FVector(0.0f, 0.0f, 80.0f), FVector(38.0f, 32.0f, 14.0f),
		Srgb(110, 70, 48), 0.4f);
	Turntable = NewObject<USceneComponent>(this);
	Turntable->SetupAttachment(RootComponent);
	Turntable->SetRelativeLocation(GramophoneAt + FVector(0.0f, 0.0f, 88.0f));
	Turntable->RegisterComponent();
	RoomShapes::Add(this, Turntable, EShape::Cylinder, FVector::ZeroVector, FVector(28.0f, 28.0f, 1.2f), Srgb(24, 24, 28), 0.2f);
	RoomShapes::Add(this, Turntable, EShape::Cylinder, FVector(0.0f, 0.0f, 0.7f), FVector(9.0f, 9.0f, 0.4f), Srgb(220, 60, 60), 0.5f);
	RoomShapes::Add(this, nullptr, EShape::Cylinder, GramophoneAt + FVector(-8.0f, 10.0f, 104.0f), FVector(2.0f, 2.0f, 30.0f), Brass, 0.3f);
	RoomShapes::Add(this, nullptr, EShape::Cone, GramophoneAt + FVector(-12.0f, 4.0f, 134.0f), FVector(44.0f, 44.0f, 40.0f), Brass, 0.25f,
		FRotator(140.0f, -60.0f, 0.0f));

	Gramophone = NewObject<URoomMusic>(this, TEXT("Gramophone"));
	Gramophone->SetupAttachment(RootComponent);
	Gramophone->SetRelativeLocation(GramophoneAt + FVector(0.0f, 0.0f, 130.0f));
	Gramophone->RegisterComponent();
	Gramophone->SetReach(400.0f, 3200.0f);
}

void ARoomThings::BuildKitchen()
{
	// A hob on the counter, with a pot on it, and steam when something's cooking.
	RoomShapes::Add(this, nullptr, EShape::Cube, FVector(325.0f, -1945.0f, 96.0f), FVector(150.0f, 62.0f, 2.0f), Srgb(36, 36, 40), 0.25f);
	RoomShapes::Add(this, nullptr, EShape::Cylinder, PotAt + FVector(0.0f, 0.0f, 9.0f), FVector(32.0f, 32.0f, 18.0f), Srgb(176, 180, 186),
		0.25f);
	RoomShapes::Add(this, nullptr, EShape::Cylinder, PotAt + FVector(0.0f, 0.0f, 18.5f), FVector(30.0f, 30.0f, 1.5f), Srgb(150, 154, 160),
		0.3f);
	RoomShapes::Add(this, nullptr, EShape::Cube, PotAt + FVector(24.0f, 0.0f, 12.0f), FVector(18.0f, 3.0f, 2.0f), Srgb(40, 40, 44), 0.5f);
	for (int32 I = 0; I < 6; ++I)
	{
		UStaticMeshComponent* Puff = RoomShapes::Add(this, nullptr, EShape::Sphere, PotAt, FVector(12.0f), Srgb(244, 244, 246), 1.0f);
		Puff->SetCastShadow(false);
		Puff->SetVisibility(false);
		Steam.Add(Puff);
	}

	// A cake stand on the island, and a cake, once one's been baked.
	RoomShapes::Add(this, nullptr, EShape::Cylinder, CakeAt + FVector(0.0f, 0.0f, 5.0f), FVector(8.0f, 8.0f, 10.0f), Srgb(236, 234, 228), 0.3f);
	RoomShapes::Add(this, nullptr, EShape::Cylinder, CakeAt + FVector(0.0f, 0.0f, 10.5f), FVector(46.0f, 46.0f, 1.5f), Srgb(240, 238, 232), 0.2f);
	Cake = NewObject<USceneComponent>(this);
	Cake->SetupAttachment(RootComponent);
	Cake->SetRelativeLocation(CakeAt + FVector(0.0f, 0.0f, 11.5f));
	Cake->SetRelativeScale3D(FVector(0.01f));
	Cake->RegisterComponent();
	RoomShapes::Add(this, Cake, EShape::Cylinder, FVector(0.0f, 0.0f, 6.0f), FVector(36.0f, 36.0f, 12.0f), Srgb(242, 222, 172), 0.7f);
	RoomShapes::Add(this, Cake, EShape::Cylinder, FVector(0.0f, 0.0f, 13.0f), FVector(37.0f, 37.0f, 2.5f), Srgb(244, 168, 190), 0.45f);
	for (int32 I = 0; I < 6; ++I)
	{
		const FVector Around = FRotator(0.0f, 60.0f * I, 0.0f).Vector() * 12.0f;
		RoomShapes::Add(this, Cake, EShape::Sphere, Around + FVector(0.0f, 0.0f, 15.5f), FVector(3.2f), Srgb(206, 30, 50), 0.25f);
	}
	Cake->SetVisibility(false, true);

	// The dinner bell, on a post by the door.
	RoomShapes::Add(this, nullptr, EShape::Cylinder, BellAt + FVector(0.0f, 0.0f, 75.0f), FVector(6.0f, 6.0f, 150.0f), Wood, 0.5f,
		FRotator::ZeroRotator, true);
	RoomShapes::Add(this, nullptr, EShape::Cube, BellAt + FVector(0.0f, 0.0f, 152.0f), FVector(30.0f, 4.0f, 4.0f), Wood, 0.5f);
	RoomShapes::Add(this, nullptr, EShape::Cone, BellAt + FVector(12.0f, 0.0f, 140.0f), FVector(18.0f, 18.0f, 18.0f), Brass, 0.2f);
	RoomShapes::Add(this, nullptr, EShape::Sphere, BellAt + FVector(12.0f, 0.0f, 129.0f), FVector(4.0f), Brass, 0.3f);
}

void ARoomThings::BuildConservatory()
{
	// Two long beds along the side walls: flowers in one, tomatoes in the other.
	FRandomStream Random(7);
	for (int32 Bed = 0; Bed < 2; ++Bed)
	{
		const FVector At = Bed == 0 ? FlowersAt : TomatoesAt;
		RoomShapes::Add(this, nullptr, EShape::Cube, At + FVector(0.0f, 0.0f, 18.0f), FVector(330.0f, 76.0f, 36.0f), Wood, 0.6f,
			FRotator::ZeroRotator, true);
		RoomShapes::Add(this, nullptr, EShape::Cube, At + FVector(0.0f, 0.0f, 36.5f), FVector(316.0f, 62.0f, 2.0f), Soil, 0.95f);
		for (int32 I = 0; I < 12; ++I)
		{
			const FVector Base = At + FVector(-140.0f + 25.5f * I + Random.FRandRange(-6.0f, 6.0f), Random.FRandRange(-18.0f, 18.0f), 37.0f);
			if (Bed == 0)
			{
				const float Tall = Random.FRandRange(26.0f, 44.0f);
				RoomShapes::Add(this, nullptr, EShape::Cylinder, Base + FVector(0.0f, 0.0f, Tall / 2), FVector(1.2f, 1.2f, Tall), Leaf, 0.7f);
				RoomShapes::Add(this, nullptr, EShape::Sphere, Base + FVector(0.0f, 0.0f, Tall), FVector(9.0f, 9.0f, 6.5f),
					RoomTypes::Blooms()[Random.RandRange(0, RoomTypes::Blooms().Num() - 1)].Color, 0.5f);
				RoomShapes::Add(this, nullptr, EShape::Sphere, Base + FVector(3.0f, 0.0f, Tall * 0.45f), FVector(7.0f, 2.0f, 3.0f), Leaf, 0.7f,
					FRotator(25.0f, Random.FRandRange(0.0f, 360.0f), 0.0f));
			}
			else if (I % 2 == 0)
			{
				RoomShapes::Add(this, nullptr, EShape::Sphere, Base + FVector(0.0f, 0.0f, 26.0f), FVector(44.0f, 40.0f, 52.0f), Leaf, 0.85f);
				for (int32 T = 0; T < 4; ++T)
				{
					const FVector Around = FRotator(0.0f, 90.0f * T + 40.0f, 0.0f).Vector() * 19.0f;
					RoomShapes::Add(this, nullptr, EShape::Sphere, Base + Around + FVector(0.0f, 0.0f, 20.0f + 7.0f * T), FVector(8.0f),
						Srgb(214, 46, 36), 0.3f);
				}
			}
		}
	}
}

FRoomSpot ARoomThings::GetPlanterSpot(int32 Index) const
{
	const float X = Index < 2 ? 1250.0f : 1950.0f;
	const float Y = Index % 2 == 0 ? -440.0f : 440.0f;
	return {FVector(X, Y, 0.0f), Y < 0.0f ? -90.0f : 90.0f};
}

//
// Music
//

void ARoomThings::SetGramophone(bool bOn)
{
	if (bOn == IsGramophoneOn())
	{
		return;
	}
	if (bOn)
	{
		Gramophone->Play(ERoomMusicStyle::Dance);
	}
	else
	{
		Gramophone->Stop();
	}
	URoomMusic::PlaySound(GetWorld(), GramophoneAt + FVector(0.0f, 0.0f, 90.0f), ERoomSound::Pop, 0.6f);
	UE_LOG(LogRoomThings, Log, TEXT("The gramophone is %s"), bOn ? TEXT("on") : TEXT("off"));
}

bool ARoomThings::IsGramophoneOn() const
{
	return Gramophone && Gramophone->IsPlaying();
}

void ARoomThings::SetPiano(bool bPlaying)
{
	if (bPlaying == IsPianoPlaying())
	{
		return;
	}
	if (bPlaying)
	{
		Piano->Play(ERoomMusicStyle::Piano);
	}
	else
	{
		Piano->Stop();
	}
	UE_LOG(LogRoomThings, Log, TEXT("The piano is %s"), bPlaying ? TEXT("playing") : TEXT("quiet"));
}

bool ARoomThings::IsPianoPlaying() const
{
	return Piano && Piano->IsPlaying();
}

void ARoomThings::PlinkPiano()
{
	if (Piano && !Piano->IsPlaying())
	{
		Piano->PlayNotes();
	}
}

FName ARoomThings::GetMusicArea() const
{
	if (IsGramophoneOn())
	{
		return TEXT("hall");
	}
	return IsPianoPlaying() ? FName(TEXT("music")) : NAME_None;
}

double ARoomThings::GetBeatAt(const FVector& Where, float& OutStrength) const
{
	OutStrength = 0.0f;
	double Beat = 0.0;
	for (const URoomMusic* Music : {Gramophone.Get(), Piano.Get()})
	{
		if (!Music || !Music->IsPlaying())
		{
			continue;
		}
		// Heard well within a few meters, and not at all a room or two away.
		const float Distance = FVector::Dist2D(Where, Music->GetComponentLocation());
		const float Heard = 1.0f - FMath::SmoothStep(700.0f, 1500.0f, Distance);
		if (Heard > OutStrength)
		{
			OutStrength = Heard;
			Beat = Music->GetBeat();
		}
	}
	return Beat;
}

float ARoomThings::GetMusicLoudness() const
{
	return FMath::Max(Gramophone ? Gramophone->GetLoudness() : 0.0f, Piano ? Piano->GetLoudness() : 0.0f);
}

void ARoomThings::Duck(float Amount)
{
	if (Gramophone)
	{
		Gramophone->SetDuck(Amount);
	}
	if (Piano)
	{
		Piano->SetDuck(Amount * 0.6f);
	}
}

//
// The kitchen, the conservatory and the gallery
//

void ARoomThings::SetCooking(bool bInCooking)
{
	bCooking = bInCooking;
}

void ARoomThings::BakeCake()
{
	if (bCake)
	{
		return;
	}
	bCake = true;
	Cake->SetVisibility(true, true);
	UE_LOG(LogRoomThings, Log, TEXT("A cake is baked"));
	URoomMusic::PlaySound(GetWorld(), CakeAt + FVector(0.0f, 0.0f, 30.0f), ERoomSound::Chime, 0.8f);
}

void ARoomThings::RingBell()
{
	URoomMusic::PlaySound(GetWorld(), BellAt + FVector(12.0f, 0.0f, 140.0f), ERoomSound::Bell, 1.0f);
}

void ARoomThings::Water(int32 Planter)
{
	if (House.IsValid())
	{
		House->Grow(Planter);
	}
}

void ARoomThings::Wish()
{
	URoomMusic::PlaySound(GetWorld(), FountainAt + FVector(60.0f, 0.0f, 50.0f), ERoomSound::Plink, 0.8f);
}

void ARoomThings::Spin(int32 Sculpture)
{
	if (Spins.IsValidIndex(Sculpture))
	{
		Spins[Sculpture] += 540.0f;
	}
}

//
// What the player can do
//

FVector ARoomThings::GetLocation(FName Thing, int32 Index) const
{
	if (Thing == TEXT("gramophone"))
	{
		return GramophoneAt;
	}
	if (Thing == TEXT("piano"))
	{
		return KeyboardAt;
	}
	if (Thing == TEXT("bell"))
	{
		return BellAt;
	}
	if (Thing == TEXT("flowers"))
	{
		return FlowersAt;
	}
	if (Thing == TEXT("tomatoes"))
	{
		return TomatoesAt;
	}
	if (Thing == TEXT("cake"))
	{
		return CakeAt;
	}
	if (Thing == TEXT("fountain"))
	{
		return FountainAt;
	}
	if (Thing == TEXT("sculpture") && House.IsValid() && Index < House->GetSculptureCount())
	{
		return House->GetSculptureLocation(Index);
	}
	return FVector::ZeroVector;
}

bool ARoomThings::FindInteraction(const FVector& Where, const FVector& Facing, ERoomItem Held, FInteraction& Out) const
{
	float Best = TNumericLimits<float>::Max();
	auto Consider = [&](FName Thing, int32 Index, float Within, const FString& Label) {
		const FVector At = GetLocation(Thing, Index);
		FVector To = At - Where;
		To.Z = 0.0f;
		const float Distance = To.Size();
		if (Distance > Within)
		{
			return;
		}
		// What's in front of the player first.
		const float Score = Distance * ((To.GetSafeNormal() | Facing.GetSafeNormal2D()) < 0.0f ? 1.8f : 1.0f);
		if (Score < Best)
		{
			Best = Score;
			Out.Label = Label;
			Out.Thing = Thing;
			Out.Index = Index;
		}
	};
	const bool bEmpty = Held == ERoomItem::None;
	Consider(TEXT("gramophone"), 0, Reach, IsGramophoneOn() ? TEXT("Stop the music") : TEXT("Put a record on"));
	Consider(TEXT("piano"), 0, 150.0f, TEXT("Play the piano"));
	Consider(TEXT("bell"), 0, 160.0f, TEXT("Ring the dinner bell"));
	if (bEmpty)
	{
		Consider(TEXT("flowers"), 0, 200.0f, TEXT("Pick a flower"));
		Consider(TEXT("tomatoes"), 0, 200.0f, TEXT("Pick a tomato"));
		if (bCake)
		{
			Consider(TEXT("cake"), 0, 190.0f, TEXT("Take a slice of cake"));
		}
	}
	Consider(TEXT("fountain"), 0, 290.0f, TEXT("Toss a coin in, and make a wish"));
	for (int32 I = 0; I < Spins.Num(); ++I)
	{
		Consider(TEXT("sculpture"), I, 150.0f, TEXT("Give it a spin"));
	}
	return Best < TNumericLimits<float>::Max();
}

//
// Each frame
//

void ARoomThings::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;

	// The record turns while it plays.
	if (IsGramophoneOn() && Turntable)
	{
		Turntable->AddRelativeRotation(FRotator(0.0f, 200.0f * DeltaSeconds, 0.0f));
	}

	// Steam rises from the pot while something's cooking.
	for (int32 I = 0; I < Steam.Num(); ++I)
	{
		const float P = FMath::Frac(Time * 0.45f + static_cast<float>(I) / Steam.Num());
		const FVector Drift(6.0f * FMath::Sin(Time * 1.3f + I), 5.0f * FMath::Cos(Time * 0.9f + 2.0f * I), 18.0f + 70.0f * P);
		Steam[I]->SetRelativeLocation(PotAt + Drift);
		Steam[I]->SetRelativeScale3D(FVector(FMath::Max(0.05f + 0.22f * P * (1.0f - P) * 4.0f * 0.5f, 0.01f)));
		Steam[I]->SetVisibility(bCooking);
	}

	// The cake pops into being once it's baked.
	CakePresence = FMath::FInterpConstantTo(CakePresence, bCake ? 1.0f : 0.0f, DeltaSeconds, 2.5f);
	const float Pop = CakePresence < 1.0f ? 1.0f + 0.2f * FMath::Sin(UE_PI * CakePresence) : 1.0f;
	Cake->SetRelativeScale3D(FVector(FMath::Max(CakePresence * Pop, 0.01f)));

	// The sculptures spin down.
	for (int32 I = 0; I < Spins.Num(); ++I)
	{
		if (FMath::Abs(Spins[I]) > 0.5f && House.IsValid())
		{
			House->SpinSculpture(I, Spins[I] * DeltaSeconds);
			Spins[I] = FMath::FInterpTo(Spins[I], 0.0f, DeltaSeconds, 0.8f);
		}
	}

	// The hall's lamps go with the music.
	if (House.IsValid())
	{
		float Strength = 0.0f;
		const double Beat = GetBeatAt(GramophoneAt, Strength);
		House->SetParty(IsGramophoneOn() ? 1.0f : 0.0f, Beat, Gramophone ? Gramophone->GetLoudness() : 0.0f);
	}
}
