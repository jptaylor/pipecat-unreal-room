//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomItem.h"

#include "RoomShapes.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"

using RoomShapes::EShape;
using RoomShapes::Srgb;

namespace
{
// Where an item sits in the right hand, in the hand's own space: its fingers
// are along -X, its palm faces +Y, and its thumb +Z, which is the item's up.
const FVector Grip(-9.0f, 3.5f, 0.5f);
const float ComeSeconds = 0.25f;
} // namespace

ARoomItem::ARoomItem()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Grip"));
	SetActorEnableCollision(false);
}

ARoomItem* ARoomItem::Make(UWorld* World, ERoomItem Kind, USkeletalMeshComponent* Hand, const FLinearColor& Tint)
{
	if (!World || Kind == ERoomItem::None)
	{
		return nullptr;
	}
	ARoomItem* Item = World->SpawnActor<ARoomItem>();
	if (!Item)
	{
		return nullptr;
	}
	Item->Kind = Kind;
	Item->Build(Tint);
	Item->SetActorScale3D(FVector(0.01f));
	Item->PutIn(Hand);
	return Item;
}

void ARoomItem::Build(const FLinearColor& Tint)
{
	USceneComponent* Root = GetRootComponent();
	switch (Kind)
	{
	case ERoomItem::Cake:
		RoomShapes::Add(this, Root, EShape::Cube, FVector(0.0f, 0.0f, 0.0f), FVector(9.0f, 9.0f, 6.0f), Srgb(242, 222, 172), 0.7f);
		RoomShapes::Add(this, Root, EShape::Cube, FVector(0.0f, 0.0f, 3.6f), FVector(9.4f, 9.4f, 1.6f), Srgb(244, 168, 190), 0.5f);
		RoomShapes::Add(this, Root, EShape::Sphere, FVector(0.0f, 0.0f, 5.4f), FVector(2.6f), Srgb(206, 30, 50), 0.25f);
		break;
	case ERoomItem::Flower:
		RoomShapes::Add(this, Root, EShape::Cylinder, FVector(0.0f, 0.0f, 6.0f), FVector(0.9f, 0.9f, 32.0f), Srgb(70, 130, 60), 0.7f);
		RoomShapes::Add(this, Root, EShape::Sphere, FVector(2.2f, 0.0f, 8.0f), FVector(5.0f, 1.5f, 2.4f), Srgb(90, 150, 70), 0.7f,
			FRotator(30.0f, 0.0f, 0.0f));
		RoomShapes::Add(this, Root, EShape::Sphere, FVector(0.0f, 0.0f, 24.0f), FVector(8.5f, 8.5f, 6.0f), Tint, 0.5f);
		RoomShapes::Add(this, Root, EShape::Sphere, FVector(0.0f, 0.0f, 26.5f), FVector(3.0f), Srgb(250, 210, 70), 0.5f);
		break;
	case ERoomItem::Tomato:
		RoomShapes::Add(this, Root, EShape::Sphere, FVector(0.0f, 0.0f, 0.0f), FVector(8.0f, 8.0f, 7.0f), Srgb(214, 46, 36), 0.3f);
		RoomShapes::Add(this, Root, EShape::Cylinder, FVector(0.0f, 0.0f, 4.2f), FVector(0.8f, 0.8f, 2.0f), Srgb(70, 120, 50), 0.7f);
		break;
	case ERoomItem::Can:
		RoomShapes::Add(this, Root, EShape::Cylinder, FVector(0.0f, -4.0f, -9.0f), FVector(13.0f, 13.0f, 15.0f), Srgb(70, 150, 150), 0.4f);
		// Its spout, sloping up and out along the fingers.
		RoomShapes::Add(this, Root, EShape::Cylinder, FVector(-12.0f, -4.0f, -5.0f), FVector(1.8f, 1.8f, 18.0f), Srgb(70, 150, 150), 0.4f,
			FRotator(55.0f, 0.0f, 0.0f));
		break;
	case ERoomItem::None:
		break;
	}
	TArray<UStaticMeshComponent*> Parts;
	GetComponents(Parts);
	for (UStaticMeshComponent* Part : Parts)
	{
		Part->SetCastShadow(true);
		Part->SetVisibleInRayTracing(false);
	}
}

void ARoomItem::PutIn(USkeletalMeshComponent* Hand)
{
	if (!Hand)
	{
		return;
	}
	const FName Socket = Hand->DoesSocketExist(TEXT("hand_r")) ? FName(TEXT("hand_r")) : NAME_None;
	AttachToComponent(Hand, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
	SetActorRelativeLocation(Grip);
	SetActorRelativeRotation(FRotator::ZeroRotator);
}

void ARoomItem::Vanish()
{
	bVanishing = true;
}

void ARoomItem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Presence = FMath::FInterpConstantTo(Presence, bVanishing ? 0.0f : 1.0f, DeltaSeconds, 1.0f / ComeSeconds);
	if (bVanishing && Presence <= 0.0f)
	{
		Destroy();
		return;
	}
	// A little overshoot as it pops in.
	const float Pop = Presence < 1.0f && !bVanishing ? 1.0f + 0.25f * FMath::Sin(UE_PI * Presence) : 1.0f;
	SetActorScale3D(FVector(FMath::Max(Presence * Pop, 0.01f)));
}
