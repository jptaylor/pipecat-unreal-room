//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

// The house's life, for the stage: what the player can do where they are, and
// does with E; what the characters do as the bot asks, e.g. dance, play the
// piano or bring the player cake; who dances to the music; and the characters
// going about their day, and visiting each other, when the player isn't
// around. What happens is told to the bot as events, with who saw or heard it.

#include "RoomStage.h"

#include "PipecatVoiceComponent.h"
#include "RoomCharacter.h"
#include "RoomHouse.h"
#include "RoomItem.h"
#include "RoomMusic.h"
#include "SRoomCaptions.h"

#include "Components/InputComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoomLife, Log, All);

namespace
{
// How close the player comes to give someone something, in cm.
const float GiveReach = 210.0f;
// How far each kind of thing that happens is seen or heard, in cm.
const float SeenFrom = 900.0f;
const float MusicHeardFrom = 1400.0f;

FString ToJson(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

ARoomStage* FindStage(UWorld* World)
{
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(World, ARoomStage::StaticClass(), Found);
	return Found.IsEmpty() ? nullptr : Cast<ARoomStage>(Found[0]);
}

// E.g. Room.Act juno play, or Room.Interact, or Room.Routine theo.
FAutoConsoleCommandWithWorldAndArgs ActCommand(
	TEXT("Room.Act"),
	TEXT("Has a character do something (dance, play, music_on, music_off, cook, food, flower, water, stop, hand, art or introduce), for "
		 "someone, and in a color, e.g. Room.Act juno play, Room.Act theo food maya, or Room.Act maya flower - yellow"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ARoomStage* Stage = FindStage(World);
		ARoomCharacter* Character = Stage && Args.Num() > 1 ? Stage->FindCharacter(Args[0]) : nullptr;
		if (Character)
		{
			ARoomCharacter* For = Args.Num() > 2 ? Stage->FindCharacter(Args[2]) : nullptr;
			Stage->Act(Character, Args[1], For != Character ? For : nullptr, Args.Num() > 3 ? Args[3] : FString());
		}
	}));

FAutoConsoleCommandWithWorldAndArgs InteractCommand(
	TEXT("Room.Interact"),
	TEXT("Does whatever the player can do where they are, as if they pressed E"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		if (ARoomStage* Stage = FindStage(World))
		{
			Stage->Interact();
		}
	}));

FAutoConsoleCommandWithWorldAndArgs RoutineCommand(
	TEXT("Room.Routine"),
	TEXT("Has a character go about their day now, e.g. Room.Routine maya, or visit another, e.g. Room.Routine juno theo"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ARoomStage* Stage = FindStage(World);
		ARoomCharacter* Character = Stage && Args.Num() > 0 ? Stage->FindCharacter(Args[0]) : nullptr;
		if (!Character)
		{
			return;
		}
		if (ARoomCharacter* Host = Args.Num() > 1 ? Stage->FindCharacter(Args[1]) : nullptr)
		{
			Stage->Visit(Character, Host);
		}
		else
		{
			Stage->Routine(Character);
		}
	}));

// A moment with whoever they've just handed something to, before going on.
FRoomStep Linger()
{
	return FRoomStep::BusyWith(ERoomActivity::None, 4.0f);
}

// A flower from the conservatory's bed, at random.
const RoomTypes::FRoomBloom& AnyBloom()
{
	const TConstArrayView<RoomTypes::FRoomBloom> Blooms = RoomTypes::Blooms();
	return Blooms[FMath::RandRange(0, Blooms.Num() - 1)];
}
} // namespace

//
// The player
//

void ARoomStage::BindInput()
{
	if (bInputBound)
	{
		return;
	}
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller)
	{
		return;
	}
	EnableInput(Controller);
	if (!InputComponent)
	{
		return;
	}
	InputComponent->BindKey(EKeys::E, IE_Pressed, this, &ARoomStage::Interact);
	InputComponent->BindKey(EKeys::Gamepad_FaceButton_Left, IE_Pressed, this, &ARoomStage::Interact);
	bInputBound = true;
}

USkeletalMeshComponent* ARoomStage::PlayerHand() const
{
	const ACharacter* Player = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	return Player ? Player->GetMesh() : nullptr;
}

FString ARoomStage::Called(const ARoomCharacter* Character) const
{
	return Known.Contains(Character->GetId()) ? Character->GetCast().Name : TEXT("them");
}

ARoomCharacter* ARoomStage::Owner(FName Area) const
{
	for (ARoomCharacter* Character : Characters)
	{
		if (Character->GetCast().Home == Area)
		{
			return Character;
		}
	}
	return nullptr;
}

void ARoomStage::PlayerHolds(ARoomItem* Item)
{
	if (PlayerItem && PlayerItem != Item)
	{
		PlayerItem->Vanish();
	}
	PlayerItem = Item;
	if (PlayerItem)
	{
		PlayerItem->PutIn(PlayerHand());
	}
}

void ARoomStage::UpdatePrompt(float DeltaSeconds)
{
	Action = EAction::None;
	GiveTarget.Reset();
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player || !Captions)
	{
		return;
	}
	const FVector Where = Player->GetActorLocation();
	const FVector Facing = Player->GetActorForwardVector();
	FString Label;

	// Holding something, someone near to give it to.
	if (PlayerItem)
	{
		float Nearest = GiveReach;
		for (ARoomCharacter* Character : Characters)
		{
			if (Character->GetHeld())
			{
				continue;  // their hands are full
			}
			FVector To = Character->GetActorLocation() - Where;
			To.Z = 0.0f;
			const float Distance = To.Size() * ((To.GetSafeNormal() | Facing) < 0.0f ? 1.6f : 1.0f);
			if (Distance < Nearest)
			{
				Nearest = Distance;
				GiveTarget = Character;
			}
		}
		if (GiveTarget.IsValid())
		{
			Action = EAction::GiveTo;
			Label = FString::Printf(TEXT("Give %s to %s"), *PlayerItem->GetName(), *Called(GiveTarget.Get()));
		}
	}
	// Something to do with a thing.
	if (Action == EAction::None && Things
		&& Things->FindInteraction(Where, Facing, PlayerItem ? PlayerItem->GetKind() : ERoomItem::None, Interaction))
	{
		Action = EAction::Thing;
		Label = Interaction.Label;
	}
	// Or with what they're holding.
	if (Action == EAction::None && PlayerItem)
	{
		const bool bFood = RoomTypes::IsFood(PlayerItem->GetKind());
		Action = bFood ? EAction::Eat : EAction::PutDown;
		Label = bFood ? FString::Printf(TEXT("Eat %s"), *PlayerItem->GetName())
					  : FString::Printf(TEXT("Put %s down"), *PlayerItem->GetName());
	}
	Captions->SetPrompt(Label);
}

void ARoomStage::Interact()
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return;
	}
	const FVector Where = Player->GetActorLocation();
	switch (Action)
	{
	case EAction::None:
		return;
	case EAction::GiveTo:
	{
		ARoomCharacter* Character = GiveTarget.Get();
		ARoomItem* Item = PlayerItem;
		if (!Character || !Item)
		{
			return;
		}
		PlayerItem = nullptr;
		Character->Hold(Item);
		URoomMusic::PlaySound(GetWorld(), Character->GetActorLocation(), ERoomSound::Chime, 0.6f);
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("to"), Character->GetId());
		Data->SetStringField(TEXT("item"), Item->GetName());
		Event(TEXT("gift"), Data, Witnesses(Where, SeenFrom));
		EatHeld(Character);
		return;
	}
	case EAction::Eat:
	case EAction::PutDown:
		if (PlayerItem)
		{
			URoomMusic::PlaySound(GetWorld(), Where, ERoomSound::Pop, 0.4f);
			PlayerItem->Vanish();
			PlayerItem = nullptr;
		}
		return;
	case EAction::Thing:
		break;
	}

	const FName Thing = Interaction.Thing;
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("who"), TEXT("user"));
	if (Thing == TEXT("gramophone"))
	{
		const bool bOn = !Things->IsGramophoneOn();
		Things->SetGramophone(bOn);
		Data->SetBoolField(TEXT("on"), bOn);
		Data->SetStringField(TEXT("place"), TEXT("the hall"));
		Event(TEXT("music"), Data, Witnesses(Things->GetLocation(Thing), MusicHeardFrom));
	}
	else if (Thing == TEXT("piano"))
	{
		Things->PlinkPiano();
		Event(TEXT("piano"), Data, Witnesses(Things->GetLocation(Thing), SeenFrom));
	}
	else if (Thing == TEXT("bell"))
	{
		Things->RingBell();
		// It's heard all over the house: everyone comes to the kitchen.
		TArray<FString> Everyone;
		TArray<ARoomCharacter*> Coming;
		for (ARoomCharacter* Character : Characters)
		{
			Everyone.Add(Character->GetId());
			if (Character->GetIntent() != ERoomIntent::Follow)
			{
				Coming.Add(Character);
			}
		}
		Move(Coming, TEXT("go"), TEXT("kitchen"));
		Event(TEXT("bell"), Data, Everyone);
	}
	else if (Thing == TEXT("flowers") || Thing == TEXT("tomatoes") || Thing == TEXT("cake"))
	{
		const ERoomItem Kind = Thing == TEXT("flowers") ? ERoomItem::Flower : (Thing == TEXT("tomatoes") ? ERoomItem::Tomato : ERoomItem::Cake);
		const RoomTypes::FRoomBloom& Bloom = AnyBloom();
		PlayerHolds(ARoomItem::Make(GetWorld(), Kind, PlayerHand(), Bloom.Color, Kind == ERoomItem::Flower ? Bloom.Name : TEXT("")));
		URoomMusic::PlaySound(GetWorld(), Where, ERoomSound::Pop, 0.5f);
		// Whose it is might have something to say about it.
		const FName Area = Kind == ERoomItem::Cake ? FName(TEXT("kitchen")) : FName(TEXT("conservatory"));
		Data->SetStringField(TEXT("item"), PlayerItem ? PlayerItem->GetName() : FString(RoomTypes::ItemName(Kind)));
		if (const ARoomCharacter* Whose = Owner(Area))
		{
			Data->SetStringField(TEXT("owner"), Whose->GetId());
		}
		Data->SetStringField(TEXT("place"), Kind == ERoomItem::Cake ? TEXT("the kitchen") : TEXT("the conservatory"));
		Event(TEXT("picked"), Data, Witnesses(Where, SeenFrom));
	}
	else if (Thing == TEXT("fountain"))
	{
		Things->Wish();
		Event(TEXT("wish"), Data, Witnesses(Where, SeenFrom));
	}
	else if (Thing == TEXT("sculpture"))
	{
		Things->Spin(Interaction.Index);
	}
}

//
// What the characters do
//

void ARoomStage::Act(ARoomCharacter* Character, const FString& What, ARoomCharacter* For, const FString& Color)
{
	if (!Character || !Things)
	{
		return;
	}
	UE_LOG(LogRoomLife, Log, TEXT("%s: sets about %s%s%s"), *Character->GetId(), *What, For ? TEXT(" for ") : TEXT(""),
		For ? *For->GetId() : TEXT(""));
	TWeakObjectPtr<ARoomThings> Stuff(Things);
	// Who it's for: the player, unless it's someone else.
	AActor* Player = For ? static_cast<AActor*>(For) : UGameplayStatics::GetPlayerPawn(this, 0);
	const FString Whom = For ? For->GetCast().Name : FString(TEXT("the person"));

	if (What == TEXT("dance"))
	{
		// Only to music: a record on the gramophone.
		if (!Things->IsGramophoneOn())
		{
			UE_LOG(LogRoomLife, Log, TEXT("%s: no music to dance to"), *Character->GetId());
			return;
		}
		Character->StopDoing();
		Character->SetDancing(true, false);
		SittingOut.Remove(Character->GetId());
	}
	else if (What == TEXT("stop"))
	{
		Character->StopDoing();
		if (Character->IsDancing())
		{
			SittingOut.Add(Character->GetId());  // this record, anyway
		}
		Character->SetDancing(false);
	}
	else if (What == TEXT("play"))
	{
		// One at the piano at a time.
		for (ARoomCharacter* Other : Characters)
		{
			if (Other != Character && Other->GetDoing() == TEXT("playing the piano"))
			{
				Other->StopDoing();
			}
		}
		const FRoomSpot Spot = Things->GetPianoSpot();
		Character->Do({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Call([Stuff]() {
							if (Stuff.IsValid())
							{
								Stuff->SetPiano(true);
							}
						}),
						  FRoomStep::BusyWith(ERoomActivity::Piano, 60.0f)},
			TEXT("playing the piano"), [Stuff]() {
				if (Stuff.IsValid())
				{
					Stuff->SetPiano(false);
				}
			});
	}
	else if (What == TEXT("music_on") || What == TEXT("music_off"))
	{
		const bool bOn = What == TEXT("music_on");
		if (!bOn && Character->GetDoing() == TEXT("playing the piano"))
		{
			Character->StopDoing();
			return;
		}
		if (bOn == Things->IsGramophoneOn())
		{
			return;
		}
		const FRoomSpot Spot = Things->GetGramophoneSpot();
		Character->Do({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Call([Stuff, bOn]() {
							if (Stuff.IsValid())
							{
								Stuff->SetGramophone(bOn);
							}
						}),
						  FRoomStep::Make(bOn ? ERoomGesture::Clap : ERoomGesture::Nod)},
			bOn ? TEXT("putting a record on the gramophone") : TEXT("turning the music off"));
	}
	else if (What == TEXT("cook") || What == TEXT("food"))
	{
		TArray<FRoomStep> Steps;
		const bool bTomato = What == TEXT("food") && Character->GetCast().Home == TEXT("conservatory");
		if (bTomato)
		{
			// The botanist brings a tomato, fresh from the vine.
			const FRoomSpot Spot = Things->GetTomatoSpot();
			Steps = {FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Make(ERoomGesture::Bow), FRoomStep::TakeItem(ERoomItem::Tomato)};
		}
		else
		{
			// Baked first, if there's no cake.
			if (What == TEXT("cook") || !Things->HasCake())
			{
				const FRoomSpot Stove = Things->GetStoveSpot();
				TWeakObjectPtr<ARoomStage> Self(this);
				TWeakObjectPtr<ARoomCharacter> Cook(Character);
				Steps.Append({FRoomStep::WalkTo(Stove.Location, Stove.Yaw), FRoomStep::Call([Stuff]() {
								  if (Stuff.IsValid())
								  {
									  Stuff->SetCooking(true);
								  }
							  }),
					FRoomStep::BusyWith(ERoomActivity::Stir, What == TEXT("cook") ? 10.0f : 7.0f), FRoomStep::Call([Stuff, Self, Cook]() {
						if (!Stuff.IsValid())
						{
							return;
						}
						Stuff->SetCooking(false);
						const bool bNew = !Stuff->HasCake();
						Stuff->BakeCake();
						if (bNew && Self.IsValid() && Cook.IsValid())
						{
							TSharedRef<FJsonObject> Baked = MakeShared<FJsonObject>();
							Baked->SetStringField(TEXT("who"), Cook->GetId());
							Baked->SetStringField(TEXT("item"), TEXT("a cake"));
							Self->Event(TEXT("baked"), Baked, Self->Witnesses(Cook->GetActorLocation(), SeenFrom));
						}
					})});
			}
			if (What == TEXT("food"))
			{
				const FRoomSpot Spot = Things->GetCakeSpot();
				Steps.Append({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::TakeItem(ERoomItem::Cake)});
			}
		}
		if (What == TEXT("food") && Player)
		{
			Steps.Append({FRoomStep::GiveTo(Player), Linger()});
		}
		Character->Do(MoveTemp(Steps), What == TEXT("cook") ? TEXT("baking a cake") : FString::Printf(TEXT("fetching %s %s"), *Whom,
			bTomato ? TEXT("a tomato") : TEXT("some cake")), [Stuff]() {
			if (Stuff.IsValid())
			{
				Stuff->SetCooking(false);
			}
		});
	}
	else if (What == TEXT("flower"))
	{
		const FRoomSpot Spot = Things->GetFlowerSpot();
		// The color asked for, if there's one like it; otherwise, whichever.
		const RoomTypes::FRoomBloom* Asked = nullptr;
		for (const RoomTypes::FRoomBloom& Each : RoomTypes::Blooms())
		{
			if (!Color.IsEmpty() && FString(Each.Name).Contains(Color))
			{
				Asked = &Each;
			}
		}
		const RoomTypes::FRoomBloom& Bloom = Asked ? *Asked : AnyBloom();
		TArray<FRoomStep> Steps = {FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Make(ERoomGesture::Bow),
			FRoomStep::TakeItem(ERoomItem::Flower, Bloom.Color, Bloom.Name)};
		if (Player)
		{
			Steps.Append({FRoomStep::GiveTo(Player), Linger()});
		}
		Character->Do(MoveTemp(Steps), FString::Printf(TEXT("picking %s %s"), *Whom, Bloom.Name));
	}
	else if (What == TEXT("water"))
	{
		TArray<FRoomStep> Steps = {FRoomStep::TakeItem(ERoomItem::Can)};
		const int32 First = FMath::RandRange(0, Things->GetPlanterCount() - 1);
		for (int32 I = 0; I < 2; ++I)
		{
			const int32 Planter = (First + I) % Things->GetPlanterCount();
			const FRoomSpot Spot = Things->GetPlanterSpot(Planter);
			Steps.Append({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::BusyWith(ERoomActivity::Water, 4.0f),
				FRoomStep::Call([Stuff, Planter]() {
					if (Stuff.IsValid())
					{
						Stuff->Water(Planter);
					}
				})});
		}
		TWeakObjectPtr<ARoomCharacter> Waterer(Character);
		Character->Do(MoveTemp(Steps), TEXT("watering the plants"), [Waterer]() {
			if (Waterer.IsValid() && Waterer->GetHeldKind() == ERoomItem::Can)
			{
				Waterer->Release()->Vanish();
			}
		});
	}
	else if (What == TEXT("art") && House.IsValid() && House->GetSculptureCount() > 0)
	{
		// Over to their favourite sculpture, to show it off with a spin.
		const int32 Count = House->GetSculptureCount();
		const int32 Art = Character->GetCast().Art;
		const int32 Piece = Art >= 0 ? Art % Count : FMath::RandRange(0, Count - 1);
		const FVector Sculpture = House->GetSculptureLocation(Piece);
		const FVector Spot = House->FindSpotBy(Sculpture, Character->GetActorLocation(), 115.0f);
		const float Yaw = (Sculpture - Spot).Rotation().Yaw;
		Character->Do({FRoomStep::WalkTo(Spot, Yaw), FRoomStep::Make(ERoomGesture::Point), FRoomStep::Call([Stuff, Piece]() {
						  if (Stuff.IsValid())
						  {
							  Stuff->Spin(Piece);
						  }
					  }),
						  FRoomStep::Make(ERoomGesture::Clap)},
			TEXT("showing the person their favourite sculpture"));
	}
	else if (What == TEXT("introduce") && For && House.IsValid())
	{
		// Over to whoever the player's to meet, to wait for the player there.
		Introductions.RemoveAll([Character](const FIntroduction& Each) { return Each.Host == Character; });
		FIntroduction& Introduction = Introductions.Add_GetRef({Character, For, Clock});
		Character->SetIntent(ERoomIntent::Wait);
		WalkToGuest(Introduction);
	}
	else if (What == TEXT("hand") && Character->GetHeld() && Player)
	{
		Character->Do({FRoomStep::GiveTo(Player), Linger()}, FString::Printf(TEXT("handing %s %s"), *Whom, *Character->GetHeld()->GetName()));
	}
}

void ARoomStage::Give(ARoomCharacter* Giver, AActor* To)
{
	ARoomItem* Item = Giver ? Giver->Release() : nullptr;
	if (!Item)
	{
		return;
	}
	URoomMusic::PlaySound(GetWorld(), Giver->GetActorLocation(), ERoomSound::Chime, 0.6f);
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("who"), Giver->GetId());
	Data->SetStringField(TEXT("item"), Item->GetName());
	ARoomCharacter* Other = Cast<ARoomCharacter>(To);
	if (Other)
	{
		Other->Hold(Item);
		Data->SetStringField(TEXT("to"), Other->GetId());
	}
	else
	{
		PlayerHolds(Item);
	}
	Event(TEXT("handed"), Data, Witnesses(Giver->GetActorLocation(), SeenFrom));
	if (Other)
	{
		EatHeld(Other);
	}
}

void ARoomStage::WalkToGuest(FIntroduction& Introduction)
{
	ARoomCharacter* Host = Introduction.Host.Get();
	ARoomCharacter* Guest = Introduction.Guest.Get();
	if (!Host || !Guest || !House.IsValid())
	{
		return;
	}
	Introduction.Walked = Clock;
	const FVector Spot = House->FindSpotBy(Guest->GetActorLocation(), Host->GetActorLocation(), 140.0f);
	const float Yaw = (Guest->GetActorLocation() - Spot).Rotation().Yaw;
	Host->Do({FRoomStep::WalkTo(Spot, Yaw)}, FString::Printf(TEXT("taking the person to meet %s"), *Guest->GetCast().Name));
}

bool ARoomStage::IsBeingIntroduced(const ARoomCharacter* Character) const
{
	return Introductions.ContainsByPredicate([Character](const FIntroduction& Each) { return Each.Guest.Get() == Character; });
}

void ARoomStage::UpdateIntroductions()
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	for (int32 I = Introductions.Num() - 1; I >= 0; --I)
	{
		FIntroduction& Each = Introductions[I];
		ARoomCharacter* Host = Each.Host.Get();
		ARoomCharacter* Guest = Each.Guest.Get();
		// Given up on, after a couple of minutes, or if the host's been sent elsewhere.
		if (!Host || !Guest || !Player || Clock - Each.Since > 150.0 || Host->GetIntent() != ERoomIntent::Wait)
		{
			Introductions.RemoveAt(I);
			continue;
		}
		if (Host->IsBusy())
		{
			continue;  // still on the way
		}
		// The guest's moved on (finished at the piano, say): after them, once
		// they've stopped.
		if (FVector::Dist2D(Host->GetActorLocation(), Guest->GetActorLocation()) > 280.0f)
		{
			if (Clock - Each.Walked > 1.5 && Guest->GetVelocity().Size2D() < 40.0f)
			{
				WalkToGuest(Each);
			}
			continue;
		}
		// Together, and the player close enough to hear them both.
		const FVector Head = PlayerHead();
		const bool bTogether = Reaches(Host->GetHeadLocation(), Head, CharacterRange, Host, Player) &&
							   Reaches(Guest->GetHeadLocation(), Head, CharacterRange, Guest, Player);
		if (!bTogether)
		{
			continue;
		}
		// The guest stops what they're doing, to meet the player.
		if (Guest->IsBusy())
		{
			Guest->StopDoing();
		}
		Guest->SetDancing(false);
		Host->TalkTo(Guest);
		Guest->LookAt(Host, 6.0f);
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("who"), Host->GetId());
		Data->SetStringField(TEXT("to"), Guest->GetId());
		Event(TEXT("introduce"), Data, Witnesses(Host->GetActorLocation(), SeenFrom));
		Introductions.RemoveAt(I);
	}
}

void ARoomStage::EatHeld(ARoomCharacter* Character)
{
	// Something to eat, they eat, if they're not busy.
	const ARoomItem* Item = Character ? Character->GetHeld() : nullptr;
	if (!Item || !RoomTypes::IsFood(Item->GetKind()) || Character->IsBusy())
	{
		return;
	}
	TWeakObjectPtr<ARoomCharacter> Eater(Character);
	Character->Do({FRoomStep::Make(ERoomGesture::Eat), FRoomStep::Call([Eater]() {
					  if (Eater.IsValid())
					  {
						  if (ARoomItem* Eaten = Eater->Release())
						  {
							  Eaten->Vanish();
						  }
					  }
				  })},
		FString::Printf(TEXT("eating %s"), *Item->GetName()));
}

//
// The house's life
//

TArray<FString> ARoomStage::Witnesses(const FVector& Where, float Carry) const
{
	TArray<FString> Out;
	const FVector At = Where + FVector(0.0f, 0.0f, 120.0f);
	for (const TObjectPtr<ARoomCharacter>& Character : Characters)
	{
		if (Reaches(At, Character->GetHeadLocation(), Carry, Character, nullptr))
		{
			Out.Add(Character->GetId());
		}
	}
	if (const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
	{
		if (Reaches(At, PlayerHead(), Carry, Player, nullptr))
		{
			Out.Add(TEXT("user"));
		}
	}
	return Out;
}

void ARoomStage::Event(const FString& Kind, const TSharedRef<FJsonObject>& Data, const TArray<FString>& HeardBy)
{
	Data->SetStringField(TEXT("kind"), Kind);
	Data->SetArrayField(TEXT("heard_by"), Strings(HeardBy));
	UE_LOG(LogRoomLife, Log, TEXT("Event %s"), *ToJson(Data));
	Send(TEXT("event"), Data);
}

void ARoomStage::UpdateLife(float DeltaSeconds)
{
	if (!Things)
	{
		return;
	}
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	bool bAnyVoice = false;
	for (ARoomCharacter* Character : Characters)
	{
		const FString& Id = Character->GetId();
		const bool bSpeaks = Speaking.Contains(Id);
		bAnyVoice |= bSpeaks;

		// The beat of the music they hear, and dancing to it, when they're not busy.
		float Strength = 0.0f;
		const double Beat = Things->GetBeatAt(Character->GetActorLocation(), Strength);
		Character->SetBeat(Beat, Strength);
		const bool bStill = Character->GetVelocity().Size2D() < 10.0f;
		if (Things->IsGramophoneOn() && Strength > 0.55f && !Character->IsBusy() && bStill && !Character->IsDancing() &&
			!SittingOut.Contains(Id) && !IsBeingIntroduced(Character))
		{
			Character->SetDancing(true, true);
		}
		else if (Character->IsDancing() && (!Things->IsGramophoneOn() || (Character->IsDancingToMusic() && Strength < 0.3f)))
		{
			// The music's stopped, or they've wandered out of earshot of it.
			Character->SetDancing(false);
		}

		// Going about their day, now and then, when the player isn't with them:
		// not in the same room, and not talking with them lately.
		if (!NextRoutine.Contains(Id))
		{
			NextRoutine.Add(Id, Clock + FMath::FRandRange(12.0f, 30.0f));
			NextVisit.Add(Id, Clock + FMath::FRandRange(70.0f, 140.0f));
		}
		if (bSpeaks)
		{
			Engaged.Add(Id, Clock);
		}
		const FVector Here = Character->GetActorLocation();
		const float FromPlayer = Player ? FVector::Dist2D(Player->GetActorLocation(), Here) : 1.0e6f;
		const bool bWithPlayer = Player && House.IsValid() && House->AreaAt(Player->GetActorLocation()) == House->AreaAt(Here);
		const double* Talked = Engaged.Find(Id);
		const bool bFree = Character->GetIntent() == ERoomIntent::Home && !Character->IsBusy() && !Character->IsDancing() && !bSpeaks &&
						   !bWithPlayer && (!Talked || Clock - *Talked > 40.0) && !IsBeingIntroduced(Character) &&
						   !BeingVisited.Contains(Id);
		if (Clock >= NextRoutine[Id])
		{
			NextRoutine[Id] = Clock + FMath::FRandRange(25.0f, 45.0f);
			if (bFree && FromPlayer > 700.0f)
			{
				Routine(Character);
			}
		}
		if (Clock >= NextVisit[Id])
		{
			NextVisit[Id] = Clock + FMath::FRandRange(100.0f, 200.0f);
			TArray<ARoomCharacter*> Hosts;
			for (ARoomCharacter* Other : Characters)
			{
				// Not someone the player's with, or talking to: they'd be interrupted.
				const double* OtherTalked = Engaged.Find(Other->GetId());
				const bool bOtherWithPlayer = Player && House.IsValid() &&
											  House->AreaAt(Player->GetActorLocation()) == House->AreaAt(Other->GetActorLocation());
				if (Other != Character && Other->GetIntent() == ERoomIntent::Home && !Other->IsBusy() && !bOtherWithPlayer &&
					(!OtherTalked || Clock - *OtherTalked > 40.0) && !IsBeingIntroduced(Other) && !BeingVisited.Contains(Other->GetId()))
				{
					Hosts.Add(Other);
				}
			}
			if (bFree && FromPlayer > 1000.0f && !Hosts.IsEmpty())
			{
				Visit(Character, Hosts[FMath::RandRange(0, Hosts.Num() - 1)]);
			}
		}
	}
	// A new record (or none): whoever sat the last one out may dance again.
	if (Things->IsGramophoneOn() != bWasGramophoneOn)
	{
		bWasGramophoneOn = Things->IsGramophoneOn();
		SittingOut.Reset();
	}

	// The music's quieter while anyone speaks over it.
	Things->Duck(bAnyVoice ? 1.0f : 0.0f);
	UpdateIntroductions();
}

void ARoomStage::Routine(ARoomCharacter* Character)
{
	TWeakObjectPtr<ARoomThings> Stuff(Things);
	const FName Home = Character->GetCast().Home;
	if (Home == TEXT("kitchen"))
	{
		// The chef bakes, or has something on the stove.
		if (!Things->HasCake() && FMath::RandBool())
		{
			Act(Character, TEXT("cook"));
			return;
		}
		const FRoomSpot Stove = Things->GetStoveSpot();
		Character->Do({FRoomStep::WalkTo(Stove.Location, Stove.Yaw), FRoomStep::Call([Stuff]() {
							if (Stuff.IsValid())
							{
								Stuff->SetCooking(true);
							}
						}),
						  FRoomStep::BusyWith(ERoomActivity::Stir, 14.0f)},
			TEXT("cooking"), [Stuff]() {
				if (Stuff.IsValid())
				{
					Stuff->SetCooking(false);
				}
			});
	}
	else if (Home == TEXT("conservatory"))
	{
		// The botanist waters the trees, or tends the flowers.
		if (FMath::RandBool())
		{
			Act(Character, TEXT("water"));
			return;
		}
		const FRoomSpot Spot = Things->GetFlowerSpot();
		Character->Do({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Make(ERoomGesture::Bow), FRoomStep::Make(ERoomGesture::Nod),
						  FRoomStep::Make(ERoomGesture::Bow)},
			TEXT("tending the flowers"));
	}
	else if (Home == TEXT("music") && !Things->IsGramophoneOn() && !Things->IsPianoPlaying())
	{
		// The musician plays the piano for a while.
		const FRoomSpot Spot = Things->GetPianoSpot();
		Character->Do({FRoomStep::WalkTo(Spot.Location, Spot.Yaw), FRoomStep::Call([Stuff]() {
							if (Stuff.IsValid())
							{
								Stuff->SetPiano(true);
							}
						}),
						  FRoomStep::BusyWith(ERoomActivity::Piano, FMath::FRandRange(25.0f, 40.0f))},
			TEXT("playing the piano"), [Stuff]() {
				if (Stuff.IsValid())
				{
					Stuff->SetPiano(false);
				}
			});
	}
}

void ARoomStage::Visit(ARoomCharacter* Visitor, ARoomCharacter* Host)
{
	if (!Visitor || !Host || !House.IsValid())
	{
		return;
	}
	// Over to them, and a chat, and then home again.
	const FVector HostAt = Host->GetActorLocation();
	const FVector Spot = House->FindSpotBy(HostAt, Visitor->GetActorLocation(), 150.0f);
	const float Yaw = (HostAt - Spot).Rotation().Yaw;
	TWeakObjectPtr<ARoomStage> Self(this);
	TWeakObjectPtr<ARoomCharacter> Guest(Visitor);
	TWeakObjectPtr<ARoomCharacter> Friend(Host);
	const FName Area = House->AreaAt(HostAt);
	const FRoomArea* Room = House->FindArea(Area);
	const FString Place = Room ? Room->Name : TEXT("the house");
	Visitor->Do({FRoomStep::WalkTo(Spot, Yaw), FRoomStep::Call([Self, Guest, Friend, Place]() {
					 if (!Self.IsValid() || !Guest.IsValid() || !Friend.IsValid())
					 {
						 return;
					 }
					 Guest->TalkTo(Friend.Get());
					 Friend->LookAt(Guest.Get(), 40.0f);
					 TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					 Data->SetStringField(TEXT("who"), Guest->GetId());
					 Data->SetStringField(TEXT("to"), Friend->GetId());
					 Data->SetStringField(TEXT("place"), Place);
					 Self->Event(TEXT("visit"), Data, Self->Witnesses(Guest->GetActorLocation(), SeenFrom));
				 }),
					FRoomStep::BusyWith(ERoomActivity::None, FMath::FRandRange(35.0f, 55.0f))},
		FString::Printf(TEXT("visiting %s in %s"), *Host->GetCast().Name, *Place), [Self, Guest, HostId = Host->GetId()]() {
			// Over, or cut short: either way, they're both free again.
			if (Guest.IsValid())
			{
				Guest->TalkTo(nullptr);
			}
			if (Self.IsValid())
			{
				Self->BeingVisited.Remove(HostId);
			}
		});
	BeingVisited.Add(Host->GetId());
}
