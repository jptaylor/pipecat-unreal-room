//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "SRoomCaptions.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBackgroundBlur.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// How wide the player's words get before they wrap, and how far up from the
// foot of the screen they are.
const float WrapWidth = 1000.0f;
const float FromFoot = 42.0f;
// The shade behind them: how tall, and how dark at the foot of the screen.
const float ShadeHeight = 250.0f;
const float ShadeDarkness = 0.62f;
// The player's words as they're heard, and once they're final; and how long
// they stay, after.
const FLinearColor Heard(0.36f, 0.38f, 0.41f);
const FLinearColor Final(0.95f, 0.96f, 0.97f);
const float HeardSeconds = 8.0f;
const float FinalSeconds = 4.0f;
const FLinearColor Shadow(0.0f, 0.0f, 0.0f, 0.45f);

FSlateFontInfo Font(const char* Typeface, int32 Size, int32 LetterSpacing = 0)
{
	FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(Typeface, Size);
	Info.LetterSpacing = LetterSpacing;
	return Info;
}

// A dark shade rising from the foot of the screen, fading to nothing: eased,
// so there's no edge where it starts.
class SRoomShade : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SRoomShade) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) {}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.0, ShadeHeight); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const float Opacity = InWidgetStyle.GetColorAndOpacityTint().A;
		const float Height = AllottedGeometry.GetLocalSize().Y;
		TArray<FSlateGradientStop> Stops;
		const int32 Count = 10;
		for (int32 Stop = 0; Stop <= Count; ++Stop)
		{
			const float Down = static_cast<float>(Stop) / Count;
			const float Dark = FMath::Pow(FMath::SmoothStep(0.0f, 1.0f, Down), 1.4f);
			Stops.Add(FSlateGradientStop(FVector2f(0.0f, Down * Height), FLinearColor(0.0f, 0.0f, 0.0f, ShadeDarkness * Dark * Opacity)));
		}
		FSlateDrawElement::MakeGradient(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Stops, Orient_Horizontal);
		return LayerId;
	}
};
} // namespace

void SRoomCaptions::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Bottom)
		[
			SNew(SBox)
			.HeightOverride(ShadeHeight)
			[
				SAssignNew(Shade, SRoomShade)
				.RenderOpacity(0.0f)
				.Visibility(EVisibility::Collapsed)
			]
		]
		// What the player can do, in the corner: the key, and what it does.
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.0f, 0.0f, 48.0f, 72.0f))
		[
			SAssignNew(PromptBlock, SBackgroundBlur)
			.BlurStrength(8.0f)
			.CornerRadius(FVector4(14.0f, 14.0f, 14.0f, 14.0f))
			.bApplyAlphaToBlur(true)
			.Padding(0.0f)
			.RenderOpacity(0.0f)
			[
				SNew(SBorder)
				.BorderImage(&PanelBrush)
				.Padding(FMargin(16.0f, 9.0f, 18.0f, 10.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(&KeyBrush)
						.Padding(FMargin(9.0f, 3.0f))
						[
							SNew(STextBlock)
							.Font(Font("Bold", 12))
							.ColorAndOpacity(FLinearColor(0.05f, 0.06f, 0.08f))
							.Text(FText::FromString(TEXT("E")))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SAssignNew(Prompt, STextBlock)
						.Font(Font("Regular", 15))
						.ColorAndOpacity(FLinearColor(0.92f, 0.94f, 0.96f))
					]
				]
			]
		]
		// A hint, or the connection's status, over the player's words.
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(40.0f, 0.0f, 40.0f, FromFoot))
		[
			SNew(SVerticalBox)
			.Visibility(EVisibility::HitTestInvisible)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SAssignNew(Status, STextBlock)
				.Visibility(EVisibility::Collapsed)
				.Font(Font("Regular", 13, 60))
				.ColorAndOpacity(FLinearColor(0.85f, 0.88f, 0.92f, 0.9f))
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(Shadow)
				.Justification(ETextJustify::Center)
				.WrapTextAt(WrapWidth)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SAssignNew(Transcript, STextBlock)
				.Visibility(EVisibility::Collapsed)
				.RenderOpacity(0.0f)
				.Font(Font("Regular", 18))
				.ColorAndOpacity(Heard)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(Shadow)
				.Justification(ETextJustify::Center)
				.LineHeightPercentage(1.1f)
				.WrapTextAt(WrapWidth)
			]
		]
	];
}

void SRoomCaptions::SetPrompt(const FString& Text)
{
	if (!Text.IsEmpty())
	{
		Prompt->SetText(FText::FromString(Text));
	}
	bPrompt = !Text.IsEmpty();
}

void SRoomCaptions::SetStatus(const FString& Text, float Seconds)
{
	Status->SetText(FText::FromString(Text));
	Status->SetVisibility(Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible);
	Status->SetRenderOpacity(1.0f);
	StatusLeft = Seconds;
}

void SRoomCaptions::SetTranscript(const FString& Text, bool bInFinal)
{
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	if (bFinal && !bInFinal)
	{
		// Something new: grey again, as it's heard.
		Whiteness = 0.0f;
	}
	bFinal = bInFinal;
	Transcript->SetText(FText::FromString(Text));
	TranscriptLeft = bFinal ? FinalSeconds : HeardSeconds;
}

void SRoomCaptions::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (StatusLeft > 0.0f)
	{
		StatusLeft -= InDeltaTime;
		if (StatusLeft <= 0.0f)
		{
			StatusLeft = -1.5f;
		}
	}
	else if (StatusLeft < 0.0f)
	{
		// Fading out over a second and a half.
		StatusLeft = FMath::Min(StatusLeft + InDeltaTime, 0.0f);
		Status->SetRenderOpacity(-StatusLeft / 1.5f);
		if (StatusLeft >= 0.0f)
		{
			Status->SetVisibility(EVisibility::Collapsed);
		}
	}

	// The player's words: in quickly, out slowly, and white once final.
	TranscriptLeft = FMath::Max(TranscriptLeft - InDeltaTime, 0.0f);
	const bool bTranscript = TranscriptLeft > 0.0f;
	TranscriptOpacity = FMath::FInterpTo(TranscriptOpacity, bTranscript ? 1.0f : 0.0f, InDeltaTime, bTranscript ? 12.0f : 4.0f);
	if (!bTranscript && TranscriptOpacity < 0.01f)
	{
		TranscriptOpacity = 0.0f;
	}
	Whiteness = FMath::FInterpTo(Whiteness, bFinal ? 1.0f : 0.0f, InDeltaTime, 8.0f);
	Transcript->SetColorAndOpacity(FMath::Lerp(Heard, Final, Whiteness));
	Transcript->SetRenderOpacity(TranscriptOpacity);
	Transcript->SetVisibility(TranscriptOpacity > 0.0f ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);

	// The shade, behind whatever's at the foot of the screen.
	const bool bStatus = Status->GetVisibility() != EVisibility::Collapsed;
	const float Wanted = FMath::Max(TranscriptOpacity, bStatus ? Status->GetRenderOpacity() : 0.0f);
	ShadeOpacity = FMath::FInterpTo(ShadeOpacity, Wanted, InDeltaTime, 6.0f);
	if (Wanted <= 0.0f && ShadeOpacity < 0.01f)
	{
		ShadeOpacity = 0.0f;
	}
	Shade->SetRenderOpacity(ShadeOpacity);
	Shade->SetVisibility(ShadeOpacity > 0.0f ? EVisibility::HitTestInvisible : EVisibility::Collapsed);

	PromptOpacity = FMath::FInterpTo(PromptOpacity, bPrompt ? 1.0f : 0.0f, InDeltaTime, 10.0f);
	PromptBlock->SetRenderOpacity(PromptOpacity);
	PromptBlock->SetVisibility(PromptOpacity > 0.01f ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
}
