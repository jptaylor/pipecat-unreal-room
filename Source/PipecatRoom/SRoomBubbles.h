//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "Brushes/SlateRoundedBoxBrush.h"
#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class APlayerController;

// What the characters say, in speech bubbles over their heads. Each hangs
// from a tail that points at whoever's speaking, as wide as their line and as
// tall as what's been said of it (the start of a long one scrolls up out of
// sight). A bubble is smaller the further away its speaker is, and shrinks
// away as they go out of earshot; nearer bubbles are drawn over further ones,
// and further ones nudged up out of their way. One for someone behind a wall
// fades out, and one for someone off the screen waits at its edge, pointing
// their way.
class SRoomBubbles : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SRoomBubbles) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, APlayerController* InPlayer);

	/**
	 * What someone's saying: all of their line, so the bubble's the shape of
	 * it, and how many of its characters have been said, which are shown.
	 * Under their name, if the player knows it (empty: no name).
	 */
	void SetLine(const FString& Key, const FString& Name, const FLinearColor& Color, const FString& Text, int32 Said);

	/** Fades someone's bubble out in so many seconds, unless they say something else by then. */
	void FadeOut(const FString& Key, float Seconds);

	/**
	 * Where someone's bubble hangs from, in the world (above their head); how
	 * well the player hears them, from 0 to 1; and whether they're in plain
	 * view of the camera.
	 */
	void Place(const FString& Key, const FVector& Anchor, float Clarity, bool bInView);

	/** Whether someone's bubble is showing, or fading. */
	bool IsShowing(const FString& Key) const;

	/** A small dark tag over what the player's looking at, e.g. "The flowers" (empty: none). */
	void SetTag(const FString& Text, const FVector& Where);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

protected:
	virtual bool ComputeVolatility() const override { return true; }

private:
	struct FLine
	{
		int32 Start = 0;
		int32 End = 0;
	};
	struct FBubble
	{
		FString Key;
		FString Name;
		FLinearColor Color = FLinearColor::White;
		FString Text;
		int32 Said = 0;
		// Its words, wrapped, and how wide the widest line, and its name, are.
		TArray<FLine> Lines;
		float TextWidth = 0.0f;
		float NameWidth = 0.0f;

		// Where it hangs from, how well its speaker's heard, and whether they're in view.
		FVector Anchor = FVector::ZeroVector;
		float Clarity = 1.0f;
		bool bInView = true;
		bool bPlaced = false;

		// Shown, until it fades out (in so many seconds); how far it's popped
		// up, how much in view, and how well heard, eased.
		bool bShown = false;
		float FadeIn = -1.0f;
		float Pop = 0.0f;
		float View = 0.0f;
		float Hearing = 0.0f;
		// Its size, unscaled, and how far its words have scrolled, eased.
		bool bSized = false;
		FVector2f Size = FVector2f::ZeroVector;
		float Scroll = 0.0f;
		// How far it's nudged up, out of the way of a nearer bubble.
		float Lift = 0.0f;

		// On the screen, this frame: whether it's drawn; how far it is from
		// the camera; where its speaker is; its scale, where it is, how big,
		// how opaque; and whether its speaker is off the screen.
		bool bVisible = false;
		bool bWasVisible = false;
		float Distance = 0.0f;
		FVector2f Tip = FVector2f::ZeroVector;
		float Scale = 1.0f;
		FVector2f Body = FVector2f::ZeroVector;
		FVector2f BodySize = FVector2f::ZeroVector;
		float Opacity = 0.0f;
		bool bOff = false;
	};

	FBubble& Find(const FString& Key);
	void Wrap(FBubble& Bubble) const;
	void PaintBubble(const FBubble& Bubble, const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, float Fade) const;
	void PaintTail(const FBubble& Bubble, const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, const FLinearColor& Tint) const;
	void PaintTag(const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, float Fade) const;

	TWeakObjectPtr<APlayerController> Player;
	TArray<FBubble> Bubbles;
	FSlateFontInfo WordsFont;
	FSlateFontInfo NameFont;
	float LineHeight = 20.0f;
	float NameHeight = 14.0f;
	FSlateRoundedBoxBrush BodyBrush = FSlateRoundedBoxBrush(FLinearColor::White, 16.0f);
	FSlateRoundedBoxBrush ShadowBrush = FSlateRoundedBoxBrush(FLinearColor::White, 19.0f);
	FSlateRoundedBoxBrush TailBrush = FSlateRoundedBoxBrush(FLinearColor::White, 2.0f);

	// The tag: what it says, and where it is, in the world; whether it's wanted, and how
	// shown; and where it is on the screen, and how big, this frame.
	FString TagText;
	FVector TagWhere = FVector::ZeroVector;
	bool bTag = false;
	float TagOpacity = 0.0f;
	bool bTagOnScreen = false;
	FVector2f TagAt = FVector2f::ZeroVector;
	FVector2f TagSize = FVector2f::ZeroVector;
	float TagTextWidth = 0.0f;
	FSlateFontInfo TagFont;
	FSlateRoundedBoxBrush TagBrush = FSlateRoundedBoxBrush(FLinearColor::White, 9.0f);
};
