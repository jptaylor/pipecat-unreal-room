//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "SRoomCaptions.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBackgroundBlur.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// How wide a line gets before it wraps, and how fast lines fade, per second.
const float WrapWidth = 1050.0f;
const float FadeSpeed = 7.0f;
// The names' colors are paler than the characters', so they read on the panel.
const float NamePaleness = 0.3f;
const FLinearColor Shadow(0.0f, 0.0f, 0.0f, 0.6f);

FSlateFontInfo Font(const char* Typeface, int32 Size, int32 LetterSpacing = 0)
{
	FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(Typeface, Size);
	Info.LetterSpacing = LetterSpacing;
	return Info;
}
} // namespace

void SRoomCaptions::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)
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
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(40.0f, 0.0f, 40.0f, 64.0f))
		[
		SNew(SVerticalBox)
		.Visibility(EVisibility::HitTestInvisible)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 12.0f)
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
			// Frosted glass: what's behind it, blurred and darkened.
			SAssignNew(Panel, SBackgroundBlur)
			.BlurStrength(10.0f)
			.CornerRadius(FVector4(18.0f, 18.0f, 18.0f, 18.0f))
			.bApplyAlphaToBlur(true)
			.Padding(0.0f)
			.RenderOpacity(0.0f)
			.Visibility(EVisibility::Collapsed)
			[
				SNew(SBorder)
				.BorderImage(&PanelBrush)
				.Padding(FMargin(34.0f, 14.0f, 34.0f, 16.0f))
				[
					SAssignNew(Lines, SVerticalBox)
				]
			]
		]
		]
	];
	// The player's line comes first.
	Row(TEXT("user"));
}

SRoomCaptions::FRow& SRoomCaptions::Row(const FString& Key)
{
	for (const TSharedPtr<FRow>& Existing : Rows)
	{
		if (Existing->Key == Key)
		{
			return *Existing;
		}
	}
	TSharedPtr<FRow> New = MakeShared<FRow>();
	New->Key = Key;
	Lines->AddSlot()
	.AutoHeight()
	.HAlign(HAlign_Center)
	[
		SAssignNew(New->Block, SVerticalBox)
		.Visibility(EVisibility::Collapsed)
		.RenderOpacity(0.0f)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SAssignNew(New->Label, STextBlock)
			.Font(Font("Bold", 10, 300))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
			.ShadowColorAndOpacity(Shadow)
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 2.0f, 0.0f, 6.0f)
		[
			SAssignNew(New->Words, STextBlock)
			.Font(Font("Regular", Key == TEXT("user") ? 17 : 21))
			.ColorAndOpacity(Key == TEXT("user") ? FLinearColor(0.84f, 0.87f, 0.9f) : FLinearColor::White)
			.ShadowOffset(FVector2D(1.0f, 1.0f))
			.ShadowColorAndOpacity(Shadow)
			.Justification(ETextJustify::Center)
			.LineHeightPercentage(1.1f)
			.WrapTextAt(WrapWidth)
		]
	];
	Rows.Add(New);
	return *New;
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

void SRoomCaptions::SetLine(const FString& Key, const FString& Name, const FLinearColor& Color, const FString& Text, float Clarity)
{
	FRow& Line = Row(Key);
	if (Text.IsEmpty())
	{
		Line.bShown = false;
		return;
	}
	Line.Label->SetText(FText::FromString(Name.ToUpper()));
	Line.Label->SetColorAndOpacity(FMath::Lerp(Color, FLinearColor::White, NamePaleness));
	Line.Words->SetText(FText::FromString(Text));
	Line.Clarity = FMath::Clamp(Clarity, 0.0f, 1.0f);
	Line.bShown = Line.Clarity > 0.02f;
	Line.FadeIn = -1.0f;
}

void SRoomCaptions::FadeOut(const FString& Key, float Seconds)
{
	FRow& Line = Row(Key);
	if (Line.bShown)
	{
		Line.FadeIn = Seconds;
	}
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

	bool bAnything = false;
	for (const TSharedPtr<FRow>& Line : Rows)
	{
		if (Line->FadeIn >= 0.0f)
		{
			Line->FadeIn -= InDeltaTime;
			if (Line->FadeIn < 0.0f)
			{
				Line->bShown = false;
			}
		}
		const float Target = Line->bShown ? Line->Clarity : 0.0f;
		Line->Opacity = FMath::FInterpTo(Line->Opacity, Target, InDeltaTime, FadeSpeed);
		if (!Line->bShown && Line->Opacity < 0.01f)
		{
			Line->Opacity = 0.0f;
		}
		Line->Block->SetRenderOpacity(Line->Opacity);
		Line->Block->SetVisibility(Line->Opacity > 0.0f ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
		bAnything |= Line->Opacity > 0.0f;
	}
	PromptOpacity = FMath::FInterpTo(PromptOpacity, bPrompt ? 1.0f : 0.0f, InDeltaTime, 10.0f);
	PromptBlock->SetRenderOpacity(PromptOpacity);
	PromptBlock->SetVisibility(PromptOpacity > 0.01f ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);

	PanelOpacity = FMath::FInterpTo(PanelOpacity, bAnything ? 1.0f : 0.0f, InDeltaTime, FadeSpeed);
	if (!bAnything && PanelOpacity < 0.01f)
	{
		PanelOpacity = 0.0f;
	}
	Panel->SetRenderOpacity(PanelOpacity);
	Panel->SetVisibility(PanelOpacity > 0.0f ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
}
