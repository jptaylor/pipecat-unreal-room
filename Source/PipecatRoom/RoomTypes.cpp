//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomTypes.h"

namespace
{
const TCHAR* const MoodNames[] = {
	TEXT("neutral"),
	TEXT("happy"),
	TEXT("laughing"),
	TEXT("excited"),
	TEXT("proud"),
	TEXT("surprised"),
	TEXT("thinking"),
	TEXT("worried"),
	TEXT("sad"),
	TEXT("angry"),
	TEXT("bored"),
	TEXT("doubtful"),
};
static_assert(UE_ARRAY_COUNT(MoodNames) == static_cast<int32>(ERoomMood::Count));

const TCHAR* const GestureNames[] = {
	TEXT("none"),
	TEXT("nod"),
	TEXT("shake"),
	TEXT("shrug"),
	TEXT("wave"),
	TEXT("point"),
	TEXT("bow"),
	TEXT("clap"),
	TEXT("facepalm"),
	TEXT("laugh"),
	TEXT("beckon"),
	TEXT("offer"),
	TEXT("eat"),
};
static_assert(UE_ARRAY_COUNT(GestureNames) == static_cast<int32>(ERoomGesture::Count));

const float GestureLengths[] = {
	0.0f, // none
	1.3f, // nod
	1.4f, // shake
	1.6f, // shrug
	2.2f, // wave
	1.8f, // point
	1.8f, // bow
	1.8f, // clap
	2.0f, // facepalm
	1.8f, // laugh
	1.8f, // beckon
	1.6f, // offer
	2.2f, // eat
};
static_assert(UE_ARRAY_COUNT(GestureLengths) == static_cast<int32>(ERoomGesture::Count));
} // namespace

ERoomMood RoomTypes::MoodFromName(const FString& Name)
{
	for (int32 i = 0; i < UE_ARRAY_COUNT(MoodNames); ++i)
	{
		if (Name.Equals(MoodNames[i], ESearchCase::IgnoreCase))
		{
			return static_cast<ERoomMood>(i);
		}
	}
	return ERoomMood::Neutral;
}

ERoomGesture RoomTypes::GestureFromName(const FString& Name)
{
	for (int32 i = 0; i < UE_ARRAY_COUNT(GestureNames); ++i)
	{
		if (Name.Equals(GestureNames[i], ESearchCase::IgnoreCase))
		{
			return static_cast<ERoomGesture>(i);
		}
	}
	return ERoomGesture::None;
}

const TCHAR* RoomTypes::MoodName(ERoomMood Mood)
{
	const int32 Index = static_cast<int32>(Mood);
	return Index >= 0 && Index < UE_ARRAY_COUNT(MoodNames) ? MoodNames[Index] : TEXT("neutral");
}

const TCHAR* RoomTypes::GestureName(ERoomGesture Gesture)
{
	const int32 Index = static_cast<int32>(Gesture);
	return Index >= 0 && Index < UE_ARRAY_COUNT(GestureNames) ? GestureNames[Index] : TEXT("none");
}

float RoomTypes::GestureSeconds(ERoomGesture Gesture)
{
	const int32 Index = static_cast<int32>(Gesture);
	return Index >= 0 && Index < UE_ARRAY_COUNT(GestureLengths) ? GestureLengths[Index] : 0.0f;
}

const TCHAR* RoomTypes::ItemName(ERoomItem Item)
{
	switch (Item)
	{
	case ERoomItem::Cake:
		return TEXT("a slice of cake");
	case ERoomItem::Flower:
		return TEXT("a flower");
	case ERoomItem::Tomato:
		return TEXT("a tomato");
	case ERoomItem::Can:
		return TEXT("a watering can");
	case ERoomItem::None:
		break;
	}
	return TEXT("nothing");
}

TConstArrayView<RoomTypes::FRoomBloom> RoomTypes::Blooms()
{
	static const FRoomBloom All[] = {
		{FLinearColor(FColor(226, 64, 80)), TEXT("a red flower")},
		{FLinearColor(FColor(246, 196, 64)), TEXT("a yellow flower")},
		{FLinearColor(FColor(240, 140, 180)), TEXT("a pink flower")},
		{FLinearColor(FColor(250, 246, 240)), TEXT("a white flower")},
		{FLinearColor(FColor(150, 110, 210)), TEXT("a purple flower")},
		{FLinearColor(FColor(250, 130, 70)), TEXT("an orange flower")},
	};
	return All;
}

bool RoomTypes::IsFood(ERoomItem Item)
{
	return Item == ERoomItem::Cake || Item == ERoomItem::Tomato;
}
