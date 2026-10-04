//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "Brushes/SlateRoundedBoxBrush.h"
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SVerticalBox;
class SWidget;

// Captions at the bottom of the screen, as in a film: a line each for the
// player and for each character who's speaking, under their name, in their
// color, on a frosted panel that fades in while there's something to read.
// Several can speak at once, each on their own line. Above them, a hint, or
// the connection's status, if there is one.
class SRoomCaptions : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRoomCaptions) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** A line on its own, e.g. a hint, or how the connection is going, faded out after so many seconds (0: kept). */
	void SetStatus(const FString& Text, float Seconds = 0.0f);

	/**
	 * What someone says, under their name, in their color, as clear as
	 * `Clarity` (from 0 to 1): a far voice is faint. The same `Key` replaces
	 * what they said before.
	 */
	void SetLine(const FString& Key, const FString& Name, const FLinearColor& Color, const FString& Text, float Clarity = 1.0f);

	/** Fades someone's line out in so many seconds, unless they say something else by then. */
	void FadeOut(const FString& Key, float Seconds);

	/** What the player can do where they are, with the E key, e.g. "Put a record on". Empty: nothing. */
	void SetPrompt(const FString& Text);

private:
	struct FRow
	{
		FString Key;
		TSharedPtr<SWidget> Block;
		TSharedPtr<STextBlock> Label;
		TSharedPtr<STextBlock> Words;
		float Clarity = 1.0f;
		float Opacity = 0.0f;
		float FadeIn = -1.0f;
		bool bShown = false;
	};
	FRow& Row(const FString& Key);

	TSharedPtr<STextBlock> Status;
	float StatusLeft = 0.0f;
	TSharedPtr<SVerticalBox> Lines;
	TSharedPtr<SWidget> Panel;
	TArray<TSharedPtr<FRow>> Rows;
	FSlateRoundedBoxBrush PanelBrush = FSlateRoundedBoxBrush(FLinearColor(0.012f, 0.012f, 0.018f, 0.42f), 18.0f);
	float PanelOpacity = 0.0f;
	TSharedPtr<SWidget> PromptBlock;
	TSharedPtr<STextBlock> Prompt;
	FSlateRoundedBoxBrush KeyBrush = FSlateRoundedBoxBrush(FLinearColor(0.9f, 0.92f, 0.95f, 0.95f), 6.0f);
	bool bPrompt = false;
	float PromptOpacity = 0.0f;
};
