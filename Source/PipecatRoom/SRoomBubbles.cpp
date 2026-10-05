//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "SRoomBubbles.h"

#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "SceneView.h"
#include "Styling/CoreStyle.h"
#include "UnrealClient.h"

namespace
{
// A bubble's words: how big, how wide before they wrap, and how many lines
// show at once (earlier ones scroll up out of sight as more are said).
const int32 WordsSize = 14;
const int32 NameSize = 9;
const float WrapWidth = 270.0f;
const int32 MostLines = 4;
const float LineSpacing = 1.1f;
// Its padding and how round it is; and its tail: how wide where it meets the
// bubble, and how long, usually, and at most (when the bubble's been nudged
// away from its speaker).
const float PadX = 15.0f;
const float PadY = 10.0f;
const float Rounding = 16.0f;
const float TailWidth = 18.0f;
const float TailLength = 18.0f;
const float LongestTail = 70.0f;
// How far its tail turns from straight out of the bubble, at most, in degrees.
const float TailTurn = 50.0f;
// How big a bubble is: full size this far from the camera, in cm (about
// someone beside the player), smaller further away, and at most and least.
const float FullSizeAt = 450.0f;
const float Perspective = 0.65f;
const float Largest = 1.15f;
const float Smallest = 0.6f;
// As its speaker goes out of earshot, it shrinks to this, as it fades away.
const float FaintSize = 0.45f;
// Off the screen, it's no bigger than this.
const float OffScreenSize = 0.8f;
// How far in from the edges of the screen bubbles stay, and apart from each other.
const float Margin = 18.0f;
const float Gap = 6.0f;
// Its paper and ink, and its name is its speaker's color, darker.
const FLinearColor Paper(0.92f, 0.905f, 0.88f, 0.97f);
const FLinearColor Ink(0.01f, 0.012f, 0.018f);
const float NameShade = 0.4f;
// The tag over what the player's looking at: small and dark.
const int32 TagFontSize = 10;
const FVector2f TagPad(9.0f, 4.0f);
const FLinearColor TagColor(0.004f, 0.004f, 0.006f, 0.8f);

float EaseOutBack(float T)
{
	const float C1 = 1.70158f;
	const float C3 = C1 + 1.0f;
	return 1.0f + C3 * FMath::Pow(T - 1.0f, 3.0f) + C1 * FMath::Pow(T - 1.0f, 2.0f);
}

TSharedRef<FSlateFontMeasure> FontMeasure()
{
	return FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
}
} // namespace

void SRoomBubbles::Construct(const FArguments& InArgs, APlayerController* InPlayer)
{
	Player = InPlayer;
	SetCanTick(true);
	WordsFont = FCoreStyle::GetDefaultFontStyle("Regular", WordsSize);
	NameFont = FCoreStyle::GetDefaultFontStyle("Bold", NameSize);
	NameFont.LetterSpacing = 120;
	const TSharedRef<FSlateFontMeasure> Measure = FontMeasure();
	LineHeight = Measure->GetMaxCharacterHeight(WordsFont) * LineSpacing;
	NameHeight = Measure->GetMaxCharacterHeight(NameFont) + 3.0f;
	TagFont = FCoreStyle::GetDefaultFontStyle("Regular", TagFontSize);
	BodyBrush = FSlateRoundedBoxBrush(FLinearColor::White, Rounding);
	ShadowBrush = FSlateRoundedBoxBrush(FLinearColor::White, Rounding + 3.0f);
}

SRoomBubbles::FBubble& SRoomBubbles::Find(const FString& Key)
{
	for (FBubble& Bubble : Bubbles)
	{
		if (Bubble.Key == Key)
		{
			return Bubble;
		}
	}
	FBubble& New = Bubbles.AddDefaulted_GetRef();
	New.Key = Key;
	return New;
}

bool SRoomBubbles::IsShowing(const FString& Key) const
{
	for (const FBubble& Bubble : Bubbles)
	{
		if (Bubble.Key == Key)
		{
			return Bubble.bShown || Bubble.Pop > 0.0f;
		}
	}
	return false;
}

void SRoomBubbles::SetTag(const FString& Text, const FVector& Where)
{
	bTag = !Text.IsEmpty();
	if (!bTag)
	{
		return;  // what it said stays, as it fades
	}
	if (Text != TagText)
	{
		TagText = Text;
		TagTextWidth = FontMeasure()->Measure(Text, TagFont).X;
	}
	TagWhere = Where;
}

void SRoomBubbles::SetLine(const FString& Key, const FString& Name, const FLinearColor& Color, const FString& Text, int32 Said)
{
	FBubble& Bubble = Find(Key);
	if (Text.IsEmpty() || Said <= 0)
	{
		Bubble.bShown = false;
		return;
	}
	if (Text != Bubble.Text)
	{
		Bubble.Text = Text;
		Wrap(Bubble);
	}
	const FString Upper = Name.ToUpper();
	if (Upper != Bubble.Name)
	{
		Bubble.Name = Upper;
		Bubble.NameWidth = Upper.IsEmpty() ? 0.0f : FontMeasure()->Measure(Upper, NameFont).X;
	}
	Bubble.Color = Color;
	Bubble.Said = FMath::Min(Said, Text.Len());
	Bubble.bShown = true;
	Bubble.FadeIn = -1.0f;
}

void SRoomBubbles::FadeOut(const FString& Key, float Seconds)
{
	FBubble& Bubble = Find(Key);
	if (Bubble.bShown)
	{
		Bubble.FadeIn = Seconds;
	}
}

void SRoomBubbles::Place(const FString& Key, const FVector& Anchor, float Clarity, bool bInView)
{
	for (FBubble& Bubble : Bubbles)
	{
		if (Bubble.Key == Key)
		{
			Bubble.Anchor = Anchor;
			Bubble.Clarity = FMath::Clamp(Clarity, 0.0f, 1.0f);
			Bubble.bInView = bInView;
			if (!Bubble.bPlaced)
			{
				// Seen and heard as it is, from the start.
				Bubble.View = bInView ? 1.0f : 0.0f;
				Bubble.Hearing = Bubble.Clarity;
			}
			Bubble.bPlaced = true;
			return;
		}
	}
}

void SRoomBubbles::Wrap(FBubble& Bubble) const
{
	// A word at a time, onto the line until it's full.
	const TSharedRef<FSlateFontMeasure> Measure = FontMeasure();
	const FString& Text = Bubble.Text;
	Bubble.Lines.Reset();
	Bubble.TextWidth = 0.0f;
	int32 Start = 0;
	while (Start < Text.Len())
	{
		while (Start < Text.Len() && FChar::IsWhitespace(Text[Start]))
		{
			++Start;
		}
		if (Start >= Text.Len())
		{
			break;
		}
		int32 End = INDEX_NONE;
		int32 Next = Start;
		while (Next < Text.Len())
		{
			int32 WordEnd = Next;
			while (WordEnd < Text.Len() && !FChar::IsWhitespace(Text[WordEnd]))
			{
				++WordEnd;
			}
			if (End != INDEX_NONE && Measure->Measure(Text, Start, WordEnd, WordsFont).X > WrapWidth)
			{
				break;
			}
			End = WordEnd;
			Next = WordEnd;
			while (Next < Text.Len() && FChar::IsWhitespace(Text[Next]))
			{
				++Next;
			}
		}
		Bubble.Lines.Add({Start, End});
		Bubble.TextWidth = FMath::Max(Bubble.TextWidth, static_cast<float>(Measure->Measure(Text, Start, End, WordsFont).X));
		Start = End;
	}
}

void SRoomBubbles::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	const float Dt = FMath::Min(InDeltaTime, 0.1f);
	const FVector2f Area = AllottedGeometry.GetLocalSize();

	// The view, as it's drawn this frame (the camera's already moved).
	APlayerController* Controller = Player.Get();
	ULocalPlayer* Local = Controller ? Controller->GetLocalPlayer() : nullptr;
	FViewport* Viewport = Local && Local->ViewportClient ? Local->ViewportClient->Viewport : nullptr;
	FSceneViewProjectionData Projection;
	const bool bView = Viewport && Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0
					   && Local->GetProjectionData(Viewport, Projection);
	const FMatrix ViewProjection = bView ? Projection.ComputeViewProjectionMatrix() : FMatrix::Identity;
	const FIntRect Rect = bView ? Projection.GetConstrainedViewRect() : FIntRect();
	const FVector2f PerPixel = bView ? Area / FVector2f(Viewport->GetSizeXY()) : FVector2f(1.0f, 1.0f);

	// The tag, where what it's on is.
	TagOpacity = FMath::FInterpTo(TagOpacity, bTag ? 1.0f : 0.0f, Dt, 12.0f);
	bTagOnScreen = false;
	if (bView && TagOpacity > 0.01f && !TagText.IsEmpty())
	{
		const FPlane Clip = ViewProjection.TransformFVector4(FVector4(TagWhere, 1.0));
		if (Clip.W > 1.0f)
		{
			TagSize = FVector2f(TagTextWidth, FontMeasure()->GetMaxCharacterHeight(TagFont)) + TagPad * 2.0f;
			const FVector2f At = FVector2f(Rect.Min.X + (0.5f + Clip.X / Clip.W * 0.5f) * Rect.Width(),
									 Rect.Min.Y + (0.5f - Clip.Y / Clip.W * 0.5f) * Rect.Height())
								 * PerPixel;
			TagAt.X = FMath::Clamp(At.X - TagSize.X * 0.5f, Margin, FMath::Max(Margin, Area.X - Margin - TagSize.X));
			TagAt.Y = FMath::Clamp(At.Y - TagSize.Y * 0.5f, Margin, FMath::Max(Margin, Area.Y - Margin - TagSize.Y));
			bTagOnScreen = true;
		}
	}

	TArray<FBubble*> Visible;
	for (FBubble& Bubble : Bubbles)
	{
		Bubble.bWasVisible = Bubble.bVisible;
		Bubble.bVisible = false;
		if (Bubble.FadeIn >= 0.0f)
		{
			Bubble.FadeIn -= Dt;
			if (Bubble.FadeIn < 0.0f)
			{
				Bubble.bShown = false;
			}
		}
		// Popping up quickly, and away a little slower.
		Bubble.Pop = FMath::Clamp(Bubble.Pop + Dt * (Bubble.bShown ? 6.0f : -4.5f), 0.0f, 1.0f);
		Bubble.View = FMath::FInterpConstantTo(Bubble.View, Bubble.bInView ? 1.0f : 0.0f, Dt, 4.0f);
		Bubble.Hearing = FMath::FInterpTo(Bubble.Hearing, Bubble.Clarity, Dt, 6.0f);
		if (Bubble.Pop <= 0.0f)
		{
			Bubble.bSized = false;
			Bubble.Lift = 0.0f;
			continue;
		}
		if (!bView || !Bubble.bPlaced || Bubble.Lines.IsEmpty())
		{
			continue;
		}

		// Its shape: as wide as its words, and as tall as what's been said of them.
		int32 Said = 0;
		for (const FLine& Line : Bubble.Lines)
		{
			Said += Line.Start < Bubble.Said ? 1 : 0;
		}
		Said = FMath::Max(Said, 1);
		const int32 Showing = FMath::Min(Said, MostLines);
		const float Header = Bubble.Name.IsEmpty() ? 0.0f : NameHeight;
		const FVector2f Size(FMath::Max(Bubble.TextWidth, Bubble.NameWidth) + PadX * 2.0f, Header + Showing * LineHeight + PadY * 2.0f);
		const float Scroll = (Said - Showing) * LineHeight;
		if (!Bubble.bSized)
		{
			Bubble.Size = Size;
			Bubble.Scroll = Scroll;
			Bubble.bSized = true;
		}
		Bubble.Size.X = FMath::FInterpTo(Bubble.Size.X, Size.X, Dt, 14.0f);
		Bubble.Size.Y = FMath::FInterpTo(Bubble.Size.Y, Size.Y, Dt, 14.0f);
		Bubble.Scroll = FMath::FInterpTo(Bubble.Scroll, Scroll, Dt, 10.0f);

		// Where its speaker is on the screen: off the side they're on, if
		// they're behind the camera.
		const FPlane Clip = ViewProjection.TransformFVector4(FVector4(Bubble.Anchor, 1.0));
		if (Clip.W > 1.0f)
		{
			Bubble.Tip = FVector2f(Rect.Min.X + (0.5f + Clip.X / Clip.W * 0.5f) * Rect.Width(),
							 Rect.Min.Y + (0.5f - Clip.Y / Clip.W * 0.5f) * Rect.Height())
						 * PerPixel;
		}
		else
		{
			Bubble.Tip = FVector2f(Clip.X >= 0.0f ? Area.X * 2.0f : -Area.X, Area.Y * 0.55f);
		}
		Bubble.bOff = Bubble.Tip.X < 0.0f || Bubble.Tip.Y < 0.0f || Bubble.Tip.X > Area.X || Bubble.Tip.Y > Area.Y;
		Bubble.Distance = FVector::Dist(Projection.ViewOrigin, Bubble.Anchor);

		// Smaller further away, and as its speaker goes out of earshot; and
		// popping up from its tail, overshooting a little.
		float Scale = FMath::Clamp(FMath::Pow(FullSizeAt / FMath::Max(Bubble.Distance, 1.0f), Perspective), Smallest, Largest);
		Scale *= FMath::Lerp(FaintSize, 1.0f, Bubble.Hearing);
		if (Bubble.bOff)
		{
			Scale = FMath::Min(Scale, OffScreenSize);
		}
		Scale *= Bubble.bShown ? FMath::Lerp(0.55f, 1.0f, EaseOutBack(Bubble.Pop)) : FMath::Lerp(0.8f, 1.0f, Bubble.Pop);
		Bubble.Scale = Scale;
		Bubble.Opacity = (Bubble.bShown ? FMath::Min(Bubble.Pop * 2.5f, 1.0f) : Bubble.Pop) * Bubble.View
						 * FMath::SmoothStep(0.0f, 0.35f, Bubble.Hearing);
		if (Bubble.Opacity < 0.01f)
		{
			continue;
		}
		Bubble.BodySize = Bubble.Size * Scale;
		Bubble.Body = FVector2f(Bubble.Tip.X - Bubble.BodySize.X * 0.5f, Bubble.Tip.Y - TailLength * Scale - Bubble.BodySize.Y);
		Bubble.bVisible = true;
		Visible.Add(&Bubble);
	}

	// Nearer first: each further one is nudged up out of the way of any
	// nearer one it would cover.
	Visible.Sort([](const FBubble& A, const FBubble& B) { return A.Distance < B.Distance; });
	TArray<FVector2f> Placed;
	for (int32 Index = 0; Index < Visible.Num(); ++Index)
	{
		FBubble& Bubble = *Visible[Index];
		float Lift = 0.0f;
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			bool bMoved = false;
			for (int32 Nearer = 0; Nearer < Index; ++Nearer)
			{
				const FVector2f Other = Placed[Nearer];
				const FVector2f OtherSize = Visible[Nearer]->BodySize;
				const FVector2f Mine(Bubble.Body.X, Bubble.Body.Y - Lift);
				const bool bAcross = Mine.X < Other.X + OtherSize.X + Gap && Mine.X + Bubble.BodySize.X + Gap > Other.X;
				const bool bUpDown = Mine.Y < Other.Y + OtherSize.Y + Gap && Mine.Y + Bubble.BodySize.Y + Gap > Other.Y;
				if (bAcross && bUpDown)
				{
					Lift = Bubble.Body.Y + Bubble.BodySize.Y + Gap - Other.Y;
					bMoved = true;
				}
			}
			if (!bMoved)
			{
				break;
			}
		}
		Bubble.Lift = Bubble.bWasVisible ? FMath::FInterpTo(Bubble.Lift, Lift, Dt, 10.0f) : Lift;
		// On the screen, whatever else.
		FVector2f At(Bubble.Body.X, Bubble.Body.Y - Bubble.Lift);
		At.X = FMath::Clamp(At.X, Margin, FMath::Max(Margin, Area.X - Margin - Bubble.BodySize.X));
		At.Y = FMath::Clamp(At.Y, Margin, FMath::Max(Margin, Area.Y - Margin - Bubble.BodySize.Y));
		Bubble.Body = At;
		Placed.Add(At);
	}
}

int32 SRoomBubbles::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// Further ones first, so nearer ones are drawn over them.
	TArray<const FBubble*> Drawn;
	for (const FBubble& Bubble : Bubbles)
	{
		if (Bubble.bVisible)
		{
			Drawn.Add(&Bubble);
		}
	}
	Drawn.Sort([](const FBubble& A, const FBubble& B) { return A.Distance > B.Distance; });
	const float Fade = InWidgetStyle.GetColorAndOpacityTint().A;
	// The tag under the bubbles.
	if (bTagOnScreen)
	{
		PaintTag(AllottedGeometry, OutDrawElements, LayerId, Fade);
		LayerId += 2;
	}
	for (const FBubble* Bubble : Drawn)
	{
		PaintBubble(*Bubble, AllottedGeometry, OutDrawElements, LayerId, Fade);
		LayerId += 4;
	}
	return LayerId;
}

void SRoomBubbles::PaintTag(const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, float Fade) const
{
	const float Alpha = TagOpacity * Fade;
	FLinearColor Back = TagColor;
	Back.A *= Alpha;
	FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(TagSize, FSlateLayoutTransform(TagAt)), &TagBrush, ESlateDrawEffect::None, Back);
	FSlateDrawElement::MakeText(Out, LayerId + 1,
		Geometry.ToPaintGeometry(TagSize - TagPad * 2.0f + FVector2f(4.0f, 0.0f), FSlateLayoutTransform(TagAt + TagPad)), TagText, TagFont,
		ESlateDrawEffect::None, FLinearColor(0.95f, 0.95f, 0.96f, Alpha));
}

void SRoomBubbles::PaintBubble(const FBubble& Bubble, const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, float Fade) const
{
	const float Scale = Bubble.Scale;
	const float Alpha = Bubble.Opacity * Fade;
	const FVector2f Size = Bubble.Size;

	// A soft shadow under it.
	for (int32 Layer = 0; Layer < 2; ++Layer)
	{
		const float Spread = Layer == 0 ? 1.5f : 5.0f;
		const float Drop = Layer == 0 ? 2.0f : 5.0f;
		FSlateDrawElement::MakeBox(Out, LayerId,
			Geometry.ToPaintGeometry(Size + FVector2f(Spread * 2.0f), FSlateLayoutTransform(Scale, Bubble.Body + FVector2f(-Spread, Drop - Spread) * Scale)),
			&ShadowBrush, ESlateDrawEffect::None, FLinearColor(0.0f, 0.0f, 0.0f, (Layer == 0 ? 0.14f : 0.07f) * Alpha));
	}

	// Its tail, then the bubble.
	FLinearColor Tint = Paper;
	Tint.A *= Alpha;
	PaintTail(Bubble, Geometry, Out, LayerId + 1, Tint);
	FSlateDrawElement::MakeBox(Out, LayerId + 2, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Scale, Bubble.Body)), &BodyBrush,
		ESlateDrawEffect::None, Tint);

	// Who's speaking, if the player knows.
	const float Header = Bubble.Name.IsEmpty() ? 0.0f : NameHeight;
	if (!Bubble.Name.IsEmpty())
	{
		FLinearColor NameColor = Bubble.Color * NameShade;
		NameColor.A = Alpha;
		FSlateDrawElement::MakeText(Out, LayerId + 3,
			Geometry.ToPaintGeometry(FVector2f(Bubble.NameWidth + 4.0f, NameHeight), FSlateLayoutTransform(Scale, Bubble.Body + FVector2f(PadX, PadY - 1.0f) * Scale)),
			Bubble.Name, NameFont, ESlateDrawEffect::None, NameColor);
	}

	// What they've said of their line, inside the bubble: lines scrolled up
	// past its top are out of sight.
	const float Top = PadY + Header;
	const float Bottom = Size.Y - PadY + 3.0f;
	if (Bottom <= Top)
	{
		return;
	}
	Out.PushClip(FSlateClippingZone(
		Geometry.ToPaintGeometry(FVector2f(Size.X, Bottom - Top + 1.0f) * Scale, FSlateLayoutTransform(Bubble.Body + FVector2f(0.0f, Top - 1.0f) * Scale))));
	FLinearColor Words = Ink;
	Words.A = Alpha;
	for (int32 Index = 0; Index < Bubble.Lines.Num(); ++Index)
	{
		const FLine& Line = Bubble.Lines[Index];
		const float Y = Top + Index * LineHeight - Bubble.Scroll;
		if (Line.Start >= Bubble.Said || Y + LineHeight < Top - 1.0f || Y > Bottom)
		{
			continue;
		}
		FSlateDrawElement::MakeText(Out, LayerId + 3,
			Geometry.ToPaintGeometry(FVector2f(WrapWidth + PadX, LineHeight), FSlateLayoutTransform(Scale, Bubble.Body + FVector2f(PadX, Y) * Scale)),
			Bubble.Text, Line.Start, FMath::Min(Line.End, Bubble.Said), WordsFont, ESlateDrawEffect::None, Words);
	}
	Out.PopClip();
}

void SRoomBubbles::PaintTail(const FBubble& Bubble, const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId, const FLinearColor& Tint) const
{
	// Out of whichever side of the bubble faces its speaker, clear of its
	// rounded corners, toward them.
	const float Scale = Bubble.Scale;
	const FVector2f Min = Bubble.Body;
	const FVector2f Max = Bubble.Body + Bubble.BodySize;
	const FVector2f Center = (Min + Max) * 0.5f;
	const FVector2f Half = Bubble.BodySize * 0.5f;
	const FVector2f ToTip = Bubble.Tip - Center;
	if (ToTip.IsNearlyZero())
	{
		return;
	}
	const float Inset = (Rounding + TailWidth * 0.5f + 1.0f) * Scale;
	FVector2f Attach;
	FVector2f Outward;
	if (FMath::Abs(ToTip.Y) * Half.X >= FMath::Abs(ToTip.X) * Half.Y)
	{
		const float Lo = Min.X + Inset;
		const float Hi = Max.X - Inset;
		Outward = FVector2f(0.0f, ToTip.Y >= 0.0f ? 1.0f : -1.0f);
		Attach = FVector2f(Lo <= Hi ? FMath::Clamp(Bubble.Tip.X, Lo, Hi) : Center.X, ToTip.Y >= 0.0f ? Max.Y : Min.Y);
	}
	else
	{
		const float Lo = Min.Y + Inset;
		const float Hi = Max.Y - Inset;
		Outward = FVector2f(ToTip.X >= 0.0f ? 1.0f : -1.0f, 0.0f);
		Attach = FVector2f(ToTip.X >= 0.0f ? Max.X : Min.X, Lo <= Hi ? FMath::Clamp(Bubble.Tip.Y, Lo, Hi) : Center.Y);
	}
	const FVector2f Toward = Bubble.Tip - Attach;
	if (FVector2f::DotProduct(Toward, Outward) < 2.0f * Scale)
	{
		// Their speaker's behind the bubble itself.
		return;
	}
	const float Reach = Toward.Size();
	FVector2f Direction = Toward / Reach;
	const float MostTurn = FMath::DegreesToRadians(TailTurn);
	if (FVector2f::DotProduct(Direction, Outward) < FMath::Cos(MostTurn))
	{
		FVector2f Side = Direction - Outward * FVector2f::DotProduct(Direction, Outward);
		Side.Normalize();
		Direction = Outward * FMath::Cos(MostTurn) + Side * FMath::Sin(MostTurn);
	}
	const float Length = FMath::Clamp(Reach, 6.0f * Scale, (Bubble.bOff ? TailLength * 1.3f : LongestTail) * Scale);

	// A square turned into a diamond, stretched toward the speaker: only the
	// half of it outside the bubble shows, as its tail.
	const float Side = TailWidth * UE_INV_SQRT_2;
	const float Stretch = Length / (Scale * TailWidth * 0.5f);
	const FVector2f Across(-Direction.Y, Direction.X);
	const FVector2f Row0 = (Direction * Stretch + Across) * UE_INV_SQRT_2;
	const FVector2f Row1 = (Direction * Stretch - Across) * UE_INV_SQRT_2;
	const FSlateRenderTransform Shape(FMatrix2x2f(Row0.X, Row0.Y, Row1.X, Row1.Y));
	const FPaintGeometry Diamond = Geometry.ToPaintGeometry(
		FVector2f(Side, Side), FSlateLayoutTransform(Scale, Attach - FVector2f(Side, Side) * (0.5f * Scale)), Shape, FVector2f(0.5f, 0.5f));

	const float Far = (LongestTail + TailWidth) * Scale * 2.0f;
	FVector2f ClipMin(Attach.X - Far, Attach.Y - Far);
	FVector2f ClipMax(Attach.X + Far, Attach.Y + Far);
	if (Outward.Y > 0.0f)
	{
		ClipMin.Y = Attach.Y - 1.0f;
	}
	else if (Outward.Y < 0.0f)
	{
		ClipMax.Y = Attach.Y + 1.0f;
	}
	else if (Outward.X > 0.0f)
	{
		ClipMin.X = Attach.X - 1.0f;
	}
	else
	{
		ClipMax.X = Attach.X + 1.0f;
	}
	Out.PushClip(FSlateClippingZone(Geometry.ToPaintGeometry(ClipMax - ClipMin, FSlateLayoutTransform(ClipMin))));
	FSlateDrawElement::MakeBox(Out, LayerId, Diamond, &TailBrush, ESlateDrawEffect::None, Tint);
	Out.PopClip();
}
