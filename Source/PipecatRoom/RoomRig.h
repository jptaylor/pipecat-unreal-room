//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "RoomTypes.h"

struct FCompactPose;

// What a character's body is doing, for the rig to pose it: how they feel,
// what they're doing with their hands, whether they're talking or listening,
// and where they look. All in the mesh's space, where the mannequin faces +Y,
// with its left to +X.
struct FRoomPoseInput
{
	// Seconds, for breathing and idle motion, and a number that varies it
	// from one character to the next.
	float Time = 0.0f;
	float Seed = 0.0f;

	// How much they feel each mood, from 0 to 1, together at most 1: the rest
	// is neutral.
	float Mood[static_cast<int32>(ERoomMood::Count)] = {};

	// A gesture, how far into it they are, in seconds, and how big it is.
	ERoomGesture Gesture = ERoomGesture::None;
	float GestureTime = 0.0f;
	float GestureStrength = 1.0f;
	// Where they point, or beckon, or wave to.
	FVector GestureDirection = FVector(0.0f, 1.0f, 0.0f);

	// How much they're talking, from 0 to 1, eased, and how loud their voice
	// is right now.
	float Talk = 0.0f;
	float Voice = 0.0f;
	// How much they're listening to someone, from 0 to 1.
	float Listen = 0.0f;
	// How much they're walking, from 0 (standing) to 1.
	float Moving = 0.0f;
	// A start of attention, from 1 down to 0, e.g. as the player's voice reaches them.
	float Attention = 0.0f;
	// How their head turns to look at something.
	FQuat Look = FQuat::Identity;

	// What they're busy with their hands, and how much, from 0 to 1, e.g.
	// easing into playing the piano.
	ERoomActivity Activity = ERoomActivity::None;
	float ActivityAmount = 0.0f;
	// How much they're dancing, from 0 to 1, and to which beat: beats of the
	// music they hear, the fraction how far into one. It's also the beat their
	// hands keep at the piano.
	float Dance = 0.0f;
	double Beat = 0.0;
	// How much they're holding something in their right hand, from 0 to 1.
	float Hold = 0.0f;
};

// A procedural rig for the mannequin: it poses its spine, head, shoulders,
// arms (with two-bone IK) and fingers on top of what its animation does, to
// show moods, gestures, talking and listening.
namespace RoomRig
{
void Apply(FCompactPose& Pose, const FRoomPoseInput& Input);
}
