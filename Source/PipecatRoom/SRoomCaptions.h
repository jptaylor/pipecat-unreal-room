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
class SWidget;

// The player's own words at the foot of the screen, over a soft dark
// gradient: grey as they're heard, and white once they're final. Above them, a
// hint, or the connection's status, if there is one; and in the corner, what
// the player can do where they are. (What the characters say is in bubbles
// over their heads: SRoomBubbles.)
class SRoomCaptions : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRoomCaptions) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** A line on its own, e.g. a hint, or how the connection is going, faded out after so many seconds (0: kept). */
	void SetStatus(const FString& Text, float Seconds = 0.0f);

	/** What the player's saying, as it's heard, and once it's final, when it fades out after a while. */
	void SetTranscript(const FString& Text, bool bFinal);

	/** What the player can do where they are, with the E key, e.g. "Put a record on". Empty: nothing. */
	void SetPrompt(const FString& Text);

private:
	TSharedPtr<STextBlock> Status;
	float StatusLeft = 0.0f;
	TSharedPtr<SWidget> Shade;
	float ShadeOpacity = 0.0f;
	TSharedPtr<STextBlock> Transcript;
	// How long the player's words stay, and how shown they are, and how
	// white (final) they are.
	float TranscriptLeft = 0.0f;
	float TranscriptOpacity = 0.0f;
	bool bFinal = false;
	float Whiteness = 0.0f;
	TSharedPtr<SWidget> PromptBlock;
	TSharedPtr<STextBlock> Prompt;
	FSlateRoundedBoxBrush PanelBrush = FSlateRoundedBoxBrush(FLinearColor(0.012f, 0.012f, 0.018f, 0.42f), 18.0f);
	FSlateRoundedBoxBrush KeyBrush = FSlateRoundedBoxBrush(FLinearColor(0.9f, 0.92f, 0.95f, 0.95f), 6.0f);
	bool bPrompt = false;
	float PromptOpacity = 0.0f;
};
