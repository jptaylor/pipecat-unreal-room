//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomCharacterAnim.h"

#include "Animation/AnimNodeBase.h"

void FRoomCharacterAnimProxy::PreUpdate(UAnimInstance* Instance, float DeltaSeconds)
{
	FAnimSingleNodeInstanceProxy::PreUpdate(Instance, DeltaSeconds);
	Input = CastChecked<URoomCharacterAnim>(Instance)->Input;
}

bool FRoomCharacterAnimProxy::Evaluate(FPoseContext& Output)
{
	FAnimSingleNodeInstanceProxy::Evaluate(Output);
	RoomRig::Apply(Output.Pose, Input);
	return true;
}

FAnimInstanceProxy* URoomCharacterAnim::CreateAnimInstanceProxy()
{
	return new FRoomCharacterAnimProxy(this);
}
