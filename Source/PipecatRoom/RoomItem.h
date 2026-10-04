//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoomTypes.h"

#include "RoomItem.generated.h"

class USkeletalMeshComponent;

// Something to carry, in someone's right hand: a slice of cake, a flower, a
// tomato or a watering can, made of the house's flat shapes. It pops into
// being, and pops away when it's eaten or put down.
UCLASS()
class ARoomItem : public AActor
{
	GENERATED_BODY()

public:
	ARoomItem();

	/** A new item, in a color (for a flower), in someone's hand, and what it's called, if not just its kind. */
	static ARoomItem* Make(UWorld* World, ERoomItem Kind, USkeletalMeshComponent* Hand, const FLinearColor& Tint = FLinearColor::Red,
		const FString& Name = FString());

	ERoomItem GetKind() const { return Kind; }

	/** What it's called, e.g. "a pink flower" or "a slice of cake". */
	FString GetName() const;

	/** Into someone else's hand. */
	void PutIn(USkeletalMeshComponent* Hand);

	/** Shrinks away, and is gone. */
	void Vanish();

	virtual void Tick(float DeltaSeconds) override;

private:
	void Build(const FLinearColor& Tint);

	ERoomItem Kind = ERoomItem::None;
	FString Name;
	// How much of it there is, from 0 to 1, as it comes and goes.
	float Presence = 0.0f;
	bool bVanishing = false;
};
