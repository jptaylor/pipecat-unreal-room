//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimSingleNodeInstanceProxy.h"
#include "CoreMinimal.h"
#include "RoomRig.h"

#include "RoomCharacterAnim.generated.h"

// Plays a character's idle and walk, and poses them on top of it with the
// rig. It evaluates on a worker thread, with a copy of what the rig needs.
struct FRoomCharacterAnimProxy : public FAnimSingleNodeInstanceProxy
{
	FRoomCharacterAnimProxy() = default;
	explicit FRoomCharacterAnimProxy(UAnimInstance* Instance) : FAnimSingleNodeInstanceProxy(Instance) {}

	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	FRoomPoseInput Input;
};

// A character's animation: the mannequin's blend of idling, walking and
// running, by how fast they go, and the rig's posing on top of it.
UCLASS(Transient, NotBlueprintable)
class URoomCharacterAnim : public UAnimSingleNodeInstance
{
	GENERATED_BODY()

public:
	/** What the rig poses them in, from the game thread. */
	FRoomPoseInput Input;

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
};
