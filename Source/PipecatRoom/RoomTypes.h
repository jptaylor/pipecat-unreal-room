//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"

// How a character feels, which their posture shows for as long as it lasts:
// the moods the bot asks Jev to pick from, for what a character says and for
// how they take what's said to them.
enum class ERoomMood : uint8
{
	Neutral,
	Happy,
	Laughing,
	Excited,
	Proud,
	Surprised,
	Thinking,
	Worried,
	Sad,
	Angry,
	Bored,
	Doubtful,
	Count
};

// Something a character does once, e.g. as they say something.
enum class ERoomGesture : uint8
{
	None,
	Nod,
	Shake,
	Shrug,
	Wave,
	Point,
	Bow,
	Clap,
	Facepalm,
	Laugh,
	Beckon,
	// Holding out what's in their hand, to give it, and taking a bite of it.
	Offer,
	Eat,
	Count
};

// Something a character is busy doing with their hands, standing at it: playing
// the piano, stirring a pot, watering a plant.
enum class ERoomActivity : uint8
{
	None,
	Piano,
	Stir,
	Water,
};

// Something a character, or the player, can carry in their hand.
enum class ERoomItem : uint8
{
	None,
	Cake,
	Flower,
	Tomato,
	Can,
};

namespace RoomTypes
{
// By the names the bot uses, e.g. "happy", or Neutral (or None) for any other.
ERoomMood MoodFromName(const FString& Name);
ERoomGesture GestureFromName(const FString& Name);
const TCHAR* MoodName(ERoomMood Mood);
const TCHAR* GestureName(ERoomGesture Gesture);

// How long a gesture takes, in seconds.
float GestureSeconds(ERoomGesture Gesture);

// What an item is called, e.g. "a slice of cake", and whether it's food.
const TCHAR* ItemName(ERoomItem Item);
bool IsFood(ERoomItem Item);
// A flower that grows in the conservatory: its color, and what it's called,
// e.g. "a pink flower".
struct FRoomBloom
{
	FLinearColor Color;
	const TCHAR* Name;
};

/** The conservatory's flowers. */
TConstArrayView<FRoomBloom> Blooms();
} // namespace RoomTypes

// One of the characters in the house, as bot/characters.json has them.
struct FRoomCast
{
	FString Id;
	FString Name;
	FLinearColor Color = FLinearColor::White;
	// "manny" or "quinn": which of the template's mannequins they are.
	FString Body;
	// The area they start in, and go back to, e.g. "kitchen".
	FName Home;
	// How tall they are, compared to the mannequin.
	float Height = 1.0f;
};
