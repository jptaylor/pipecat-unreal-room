//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomStage.h"

#include "PipecatVoiceComponent.h"
#include "RoomCharacter.h"
#include "RoomHouse.h"
#include "RoomItem.h"
#include "RoomVoiceRing.h"
#include "SRoomCaptions.h"

#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoomStage, Log, All);

namespace
{
const FLinearColor UserColor(0.72f, 0.9f, 1.0f);

// The player's voice starts this many dB over the room's own noise, kept up
// for a moment (a click or a bump isn't a voice), and stops once it's been
// under this many for a moment. While a character's voice is playing (which
// the microphone may pick up), it has to be this much louder to start.
const float SpeechOver = 12.0f;
const float SilenceUnder = 7.0f;
const float OnsetSeconds = 0.09f;
const float HangoverSeconds = 0.35f;
const float OverVoices = 6.0f;
// A new utterance, after this long quiet: who heard it starts again.
const float NewUtteranceAfter = 1.5f;
// Every 6 dB louder, a voice carries twice as far.
const float DoublingDb = 6.0f;

// How long a mood lasts, by default, as someone speaks or listens.
const float SpeakingMoodSeconds = 6.0f;
const float ListeningMoodSeconds = 4.0f;
// How long someone's caption stays once they've stopped, in seconds.
const float CaptionHold = 3.0f;

FString ToJson(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

TArray<TSharedPtr<FJsonValue>> Strings(TArray<FString> Values)
{
	Values.Sort();
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

// A field that's a string or a list of them.
TArray<FString> StringList(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
{
	TArray<FString> Out;
	FString One;
	const TArray<TSharedPtr<FJsonValue>>* Many = nullptr;
	if (Json->TryGetArrayField(Field, Many))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Many)
		{
			FString Item;
			if (Value.IsValid() && Value->TryGetString(Item))
			{
				Out.Add(Item);
			}
		}
	}
	else if (Json->TryGetStringField(Field, One))
	{
		Out.Add(One);
	}
	return Out;
}

ARoomStage* FindStage(UWorld* World)
{
	for (TActorIterator<ARoomStage> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

// For trying it out without speaking, e.g. Room.Say Maya, can you follow me?
FAutoConsoleCommandWithWorldAndArgs SayCommand(
	TEXT("Room.Say"),
	TEXT("Says something to the bot as the player, as if spoken at their usual loudness"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		if (ARoomStage* Stage = FindStage(World))
		{
			Stage->Say(FString::Join(Args, TEXT(" ")));
		}
	}));

// E.g. Room.Speak 3 6: three seconds of speech, 6 dB louder than usual.
FAutoConsoleCommandWithWorldAndArgs SpeakCommand(
	TEXT("Room.Speak"),
	TEXT("Pretends the player speaks for so many seconds, so many dB louder than usual, e.g. Room.Speak 3 6"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		if (ARoomStage* Stage = FindStage(World))
		{
			Stage->Pretend(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.0f, Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f);
		}
	}));

// E.g. Room.Emote maya laughing clap, or Room.Move theo follow.
FAutoConsoleCommandWithWorldAndArgs EmoteCommand(
	TEXT("Room.Emote"),
	TEXT("Has a character feel a mood and make a gesture, e.g. Room.Emote maya thinking nod"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ARoomStage* Stage = FindStage(World);
		if (!Stage || Args.Num() < 2)
		{
			return;
		}
		Stage->HandleMessage(FString::Printf(TEXT("{\"type\": \"emote\", \"who\": \"%s\", \"mood\": \"%s\", \"gesture\": \"%s\", \"strength\": 1, \"seconds\": 30}"),
			*Args[0], *Args[1], Args.Num() > 2 ? *Args[2] : TEXT("none")));
	}));

FAutoConsoleCommandWithWorldAndArgs MoveCommand(
	TEXT("Room.Move"),
	TEXT("Has a character follow the player, come over, wait, go home, or go to an area, e.g. Room.Move juno go kitchen"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ARoomStage* Stage = FindStage(World);
		if (!Stage || Args.Num() < 2)
		{
			return;
		}
		Stage->HandleMessage(FString::Printf(TEXT("{\"type\": \"move\", \"who\": [\"%s\"], \"action\": \"%s\", \"area\": \"%s\"}"),
			*Args[0], *Args[1], Args.Num() > 2 ? *Args[2] : TEXT("")));
	}));

// E.g. Room.Caption theo Name's Theo. What's yours?
FAutoConsoleCommandWithWorldAndArgs CaptionCommand(
	TEXT("Room.Caption"),
	TEXT("Shows a line as if a character (or the user) said it, e.g. Room.Caption theo Hello there"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ARoomStage* Stage = FindStage(World);
		if (!Stage || Args.Num() < 2)
		{
			return;
		}
		TArray<FString> Words(Args);
		const FString Who = Words[0];
		Words.RemoveAt(0);
		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetStringField(TEXT("type"), TEXT("line"));
		Line->SetStringField(TEXT("id"), FString::Printf(TEXT("test-%s"), *Who));
		Line->SetStringField(TEXT("speaker"), Who);
		Line->SetStringField(TEXT("text"), FString::Join(Words, TEXT(" ")));
		Stage->HandleMessage(ToJson(Line));
	}));

FAutoConsoleCommandWithWorldAndArgs MessageCommand(
	TEXT("Room.Message"),
	TEXT("Handles a message as if from the bot, e.g. Room.Message {\"type\": \"line\", \"speaker\": \"maya\", \"text\": \"Hi\"}"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		if (ARoomStage* Stage = FindStage(World))
		{
			Stage->HandleMessage(FString::Join(Args, TEXT(" ")));
		}
	}));
} // namespace

ARoomStage::ARoomStage()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ARoomStage::BeginPlay()
{
	Super::BeginPlay();

	for (TActorIterator<ARoomHouse> It(GetWorld()); It; ++It)
	{
		House = *It;
	}
	if (!LoadCast())
	{
		return;
	}
	Things = GetWorld()->SpawnActor<ARoomThings>();
	if (Things && House.IsValid())
	{
		Things->Build(House.Get());
		House->AddBlockers(Things);
	}
	SpawnCharacters();
	for (ARoomCharacter* Character : Characters)
	{
		Character->OnGive = [this](ARoomCharacter* Giver, AActor* To) { Give(Giver, To); };
	}

	Ring = NewObject<URoomVoiceRing>(this, TEXT("VoiceRing"));
	Ring->SetupAttachment(RootComponent);
	Ring->RegisterComponent();

	AddCaptions();
	ConnectVoice();
}

void ARoomStage::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Captions)
	{
		if (UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
		{
			Viewport->RemoveViewportWidgetContent(Captions.ToSharedRef());
		}
		Captions.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

bool ARoomStage::LoadCast()
{
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), CastFile);
	FString Text;
	TArray<TSharedPtr<FJsonValue>> Entries;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Entries))
	{
		UE_LOG(LogRoomStage, Error, TEXT("Unable to read the characters from %s"), *Path);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(Object))
		{
			continue;
		}
		FRoomCast Member;
		(*Object)->TryGetStringField(TEXT("id"), Member.Id);
		(*Object)->TryGetStringField(TEXT("name"), Member.Name);
		FString Hex = TEXT("#ffffff");
		(*Object)->TryGetStringField(TEXT("hex"), Hex);
		Member.Color = FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
		(*Object)->TryGetStringField(TEXT("body"), Member.Body);
		FString Home;
		(*Object)->TryGetStringField(TEXT("home"), Home);
		Member.Home = FName(*Home);
		double Height = 1.0;
		if ((*Object)->TryGetNumberField(TEXT("height"), Height))
		{
			Member.Height = static_cast<float>(Height);
		}
		Members.Add(Member);
	}
	UE_LOG(LogRoomStage, Log, TEXT("%d characters, from %s"), Members.Num(), *Path);
	return !Members.IsEmpty();
}

void ARoomStage::SpawnCharacters()
{
	for (const FRoomCast& Member : Members)
	{
		FRoomSpot Home = House.IsValid() ? House->GetHome(Member.Home) : FRoomSpot {FVector::ZeroVector, 0.0f};
		const FTransform Transform(FRotator(0.0f, Home.Yaw, 0.0f), Home.Location + FVector(0.0f, 0.0f, 95.0f));
		ARoomCharacter* Character = GetWorld()->SpawnActorDeferred<ARoomCharacter>(
			ARoomCharacter::StaticClass(), Transform, this, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
		if (!Character)
		{
			continue;
		}
		Character->Setup(Member, House.Get());
		Character->FinishSpawning(Transform);
		Characters.Add(Character);
	}
}

void ARoomStage::ConnectVoice()
{
	// A voice per character, in their head, muffled through walls.
	Voice = NewObject<UPipecatVoiceComponent>(this, TEXT("Voice"));
	if (!StartUrl.IsEmpty())
	{
		Voice->StartUrl = StartUrl;
	}
	Voice->ApiKey = ApiKey;
	Voice->bUseMicrophone = bUseMicrophone;
	Voice->VoiceVolume = VoiceVolume;
	Voice->VoiceChannels = FMath::Max(Characters.Num(), 1);
	Voice->VoiceInnerRadius = 200.0f;
	Voice->VoiceFalloffDistance = 2400.0f;
	Voice->bVoiceOcclusion = true;
	Voice->OnServerMessage.AddDynamic(this, &ARoomStage::HandleServerMessage);
	Voice->OnUserTranscript.AddDynamic(this, &ARoomStage::HandleUserTranscript);
	Voice->OnBotReady.AddDynamic(this, &ARoomStage::HandleBotReady);
	Voice->OnDisconnected.AddDynamic(this, &ARoomStage::HandleDisconnected);
	Voice->OnError.AddDynamic(this, &ARoomStage::HandleError);
	Voice->RegisterComponent();
	for (int32 Index = 0; Index < Characters.Num(); ++Index)
	{
		Voice->AttachVoiceChannelTo(Index, Characters[Index]->GetVoiceParent(), Characters[Index]->GetVoiceSocket());
	}
	UE_LOG(LogRoomStage, Log, TEXT("Connecting to the bot at %s"), *Voice->StartUrl);
	if (Captions && bShowStatus)
	{
		Captions->SetStatus(TEXT("Connecting..."));
	}
}

void ARoomStage::AddCaptions()
{
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (!Viewport)
	{
		return;
	}
	Captions = SNew(SRoomCaptions);
	Viewport->AddViewportWidgetContent(Captions.ToSharedRef(), 10);
	Captions->SetStatus(TEXT("Walk around and talk to whoever you find. Speak up to be heard further away."), 9.0f);
}

//
// Each frame
//

void ARoomStage::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Clock += DeltaSeconds;
	BindInput();
	UpdateSpeech(DeltaSeconds);
	UpdateEarshot(DeltaSeconds);
	UpdateCharacters(DeltaSeconds);
	UpdateLife(DeltaSeconds);
	UpdatePrompt(DeltaSeconds);
	UpdateSpace(DeltaSeconds);
}

FVector ARoomStage::PlayerHead() const
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	return Player ? Player->GetPawnViewLocation() : FVector::ZeroVector;
}

bool ARoomStage::Reaches(const FVector& From, const FVector& To, float Carry, const AActor* Ignore, const AActor* Ignore2) const
{
	const float Distance = FVector::Dist(From, To);
	if (Distance > Carry)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RoomEarshot), false);
	Params.AddIgnoredActor(Ignore);
	Params.AddIgnoredActor(Ignore2);
	for (const TObjectPtr<ARoomCharacter>& Character : Characters)
	{
		Params.AddIgnoredActor(Character);
	}
	if (const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
	{
		Params.AddIgnoredActor(Player);
	}
	FHitResult Hit;
	const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params);
	return !bBlocked || Distance <= Carry * ThroughWalls;
}

void ARoomStage::UpdateSpeech(float DeltaSeconds)
{
	// How loud the microphone is, in dB, its peaks held a moment.
	const float Rms = Voice ? Voice->GetMicrophoneRms() : 0.0f;
	Level = 20.0f * FMath::LogX(10.0f, Rms + 1.e-5f);
	if (Voice)
	{
		Voice->GetMicrophoneWaveform(Waveform, 384);
	}
	if (PretendLeft > 0.0f)
	{
		// Pretend speech, for trying it out: a voice wavering about this loud.
		PretendLeft -= DeltaSeconds;
		const float T = GetWorld()->GetTimeSeconds();
		Level = Usual + PretendLoudness + 3.0f * FMath::Sin(T * 7.0f) * FMath::Sin(T * 2.3f);
		Waveform.SetNumUninitialized(384);
		for (int32 I = 0; I < Waveform.Num(); ++I)
		{
			const float S = I / 16000.0f + T;
			Waveform[I] = 0.2f * FMath::Sin(S * 2.0f * UE_PI * 180.0f) + 0.12f * FMath::Sin(S * 2.0f * UE_PI * 410.0f)
				+ 0.05f * FMath::Sin(S * 2.0f * UE_PI * 1130.0f);
		}
	}

	// The room's own noise, from the level smoothed over a moment, so the dips
	// between its sounds don't count as the room going quiet: down to it
	// fairly quickly, and back up more slowly while the player isn't speaking.
	Smoothed = FMath::FInterpTo(Smoothed, Level, DeltaSeconds, 2.5f);
	if (Smoothed < Floor)
	{
		Floor += (Smoothed - Floor) * FMath::Min(1.0f, 1.5f * DeltaSeconds);
	}
	else if (!bSpeaking)
	{
		Floor += (Smoothed - Floor) * FMath::Min(1.0f, 0.4f * DeltaSeconds);
	}
	Floor = FMath::Clamp(Floor, -90.0f, -38.0f);

	const bool bWasSpeaking = bSpeaking;
	const float Start = FMath::Max(Floor + SpeechOver, SpeechMinDb) + (Speaking.IsEmpty() ? 0.0f : OverVoices);
	const float Stop = FMath::Max(Floor + SilenceUnder, SpeechMinDb - 6.0f);
	Onset = Level > Start ? Onset + DeltaSeconds : 0.0f;
	if (Level > Start && (bSpeaking || Onset >= OnsetSeconds || PretendLeft > 0.0f))
	{
		bSpeaking = true;
		Hangover = HangoverSeconds;
	}
	else if (Level < Stop)
	{
		Hangover -= DeltaSeconds;
		if (Hangover <= 0.0f)
		{
			bSpeaking = false;
		}
	}
	if (bSpeaking && !bWasSpeaking && Quiet > NewUtteranceAfter)
	{
		// Something new: who hears it starts again.
		Heard.Reset();
		LastEarshot.Reset();
	}
	Quiet = bSpeaking ? 0.0f : Quiet + DeltaSeconds;

	// How far it carries: twice as far for every 6 dB louder than usual.
	Peak = FMath::Max(Level, Peak - 18.0f * DeltaSeconds);
	if (bSpeaking)
	{
		Usual += (Peak - Usual) * FMath::Min(1.0f, 0.06f * DeltaSeconds);
		Usual = FMath::Clamp(Usual, -50.0f, -12.0f);
	}
	const float Louder = Peak - Usual;
	const float Carry = FMath::Clamp(NormalRange * FMath::Pow(2.0f, Louder / DoublingDb), MinRange, MaxRange);
	const float Target = bSpeaking ? Carry : MinRange * 0.5f;
	Range = FMath::FInterpTo(Range, Target, DeltaSeconds, Target > Range ? 7.0f : 1.5f);
	Presence = FMath::FInterpTo(Presence, bSpeaking ? 1.0f : 0.0f, DeltaSeconds, bSpeaking ? 7.0f : 2.2f);

	if (Ring)
	{
		const ACharacter* Player = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
		if (Player)
		{
			const float Feet = Player->GetCapsuleComponent() ? Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.0f;
			const float Loudness = FMath::Clamp((Louder + 6.0f) / 18.0f, 0.0f, 1.0f);
			Ring->Draw(DeltaSeconds, Player->GetActorLocation() - FVector(0.0f, 0.0f, Feet - 2.0f), Range, Presence, Loudness, Waveform);
		}
	}
}

void ARoomStage::UpdateEarshot(float DeltaSeconds)
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const FVector Head = PlayerHead();
	for (const TObjectPtr<ARoomCharacter>& Character : Characters)
	{
		const bool bHears = bSpeaking && Player && Reaches(Head, Character->GetHeadLocation(), Range, Player, Character);
		Character->SetHearsPlayer(bHears);
		if (bHears)
		{
			Heard.Add(Character->GetId());
			Engaged.Add(Character->GetId(), Clock);
		}
	}
	if (!bReady || !bSpeaking)
	{
		return;
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("heard"), Strings(Heard.Array()));
	const FString Json = ToJson(Data);
	if (Json != LastEarshot)
	{
		LastEarshot = Json;
		Data->SetNumberField(TEXT("range"), FMath::RoundToInt(Range));
		Send(TEXT("earshot"), Data);
	}
}

void ARoomStage::UpdateCharacters(float DeltaSeconds)
{
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	for (int32 Index = 0; Index < Characters.Num(); ++Index)
	{
		ARoomCharacter* Character = Characters[Index];
		const float ChannelLevel = Voice ? Voice->GetBotVoiceChannelLevel(Index) : 0.0f;
		Character->SetVoiceLevel(ChannelLevel);
		// Whose voice is playing, now and then, to tell the voices went where they should.
		const double Now = FPlatformTime::Seconds();
		double& Logged = LoggedVoice.FindOrAdd(Character->GetId());
		if (ChannelLevel > 0.05f && Now - Logged > 2.0)
		{
			Logged = Now;
			UE_LOG(LogRoomStage, Log, TEXT("%s's voice is playing, from channel %d (level %.2f)"), *Character->GetId(), Index, ChannelLevel);
		}
	}

	// Who each one listens to: someone speaking within earshot, the nearest,
	// or the player, while they're heard.
	for (ARoomCharacter* Character : Characters)
	{
		if (Speaking.Contains(Character->GetId()))
		{
			Character->SetListening(nullptr);
			continue;
		}
		AActor* Listen = nullptr;
		float Nearest = TNumericLimits<float>::Max();
		for (ARoomCharacter* Other : Characters)
		{
			if (Other == Character || !Speaking.Contains(Other->GetId()))
			{
				continue;
			}
			const float Distance = FVector::Dist(Other->GetActorLocation(), Character->GetActorLocation());
			if (Distance < Nearest && Reaches(Other->GetHeadLocation(), Character->GetHeadLocation(), CharacterRange, Other, Character))
			{
				Nearest = Distance;
				Listen = Other;
			}
		}
		if (!Listen && Player && (bSpeaking || Quiet < 2.0f) && Heard.Contains(Character->GetId()))
		{
			Listen = Player;
		}
		Character->SetListening(Listen);
	}
}

void ARoomStage::UpdateSpace(float DeltaSeconds)
{
	SpaceIn -= DeltaSeconds;
	if (!bReady || SpaceIn > 0.0f || !House.IsValid())
	{
		return;
	}
	SpaceIn = 0.25f;
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return;
	}
	const FVector Head = PlayerHead();

	// Where everyone is, who can hear each character, and what each is doing.
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> User = MakeShared<FJsonObject>();
	User->SetStringField(TEXT("area"), House->AreaAt(Player->GetActorLocation()).ToString());
	Data->SetObjectField(TEXT("user"), User);
	TSharedRef<FJsonObject> Everyone = MakeShared<FJsonObject>();
	for (ARoomCharacter* Character : Characters)
	{
		TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("area"), House->AreaAt(Character->GetActorLocation()).ToString());
		TArray<FString> Hears;
		for (ARoomCharacter* Other : Characters)
		{
			if (Other != Character && Reaches(Character->GetHeadLocation(), Other->GetHeadLocation(), CharacterRange, Character, Other))
			{
				Hears.Add(Other->GetId());
			}
		}
		if (Reaches(Character->GetHeadLocation(), Head, CharacterRange, Character, Player))
		{
			Hears.Add(TEXT("user"));
		}
		One->SetArrayField(TEXT("hears"), Strings(Hears));
		One->SetStringField(TEXT("intent"), Character->GetIntentName());
		// What they're doing, and holding.
		FString Doing = Character->GetDoing();
		if (Doing.IsEmpty() && Character->IsDancing())
		{
			Doing = TEXT("dancing");
		}
		One->SetStringField(TEXT("doing"), Doing);
		One->SetStringField(TEXT("holding"), Character->GetHeld() ? RoomTypes::ItemName(Character->GetHeldKind()) : TEXT(""));
		Everyone->SetObjectField(Character->GetId(), One);

		// Coming up to someone for the first time, they greet the player.
		if (!Met.Contains(Character->GetId())
			&& Reaches(Head, Character->GetHeadLocation(), MeetDistance, Player, Character)
			&& FVector::Dist(Head, Character->GetHeadLocation()) <= MeetDistance * ThroughWalls * 1.6f)
		{
			Met.Add(Character->GetId());
			TSharedRef<FJsonObject> Meeting = MakeShared<FJsonObject>();
			Meeting->SetStringField(TEXT("who"), Character->GetId());
			Send(TEXT("met"), Meeting);
		}
	}
	Data->SetObjectField(TEXT("characters"), Everyone);
	// The house: music playing, a cake on the island, and what the player's holding.
	TSharedRef<FJsonObject> World = MakeShared<FJsonObject>();
	World->SetStringField(TEXT("music"), Things ? Things->GetMusicArea().ToString() : TEXT(""));
	if (World->GetStringField(TEXT("music")) == TEXT("None"))
	{
		World->SetStringField(TEXT("music"), TEXT(""));
	}
	World->SetBoolField(TEXT("cake"), Things && Things->HasCake());
	World->SetStringField(TEXT("holding"), PlayerItem ? RoomTypes::ItemName(PlayerItem->GetKind()) : TEXT(""));
	Data->SetObjectField(TEXT("world"), World);
	const FString Json = ToJson(Data);
	if (Json != LastSpace)
	{
		LastSpace = Json;
		Send(TEXT("space"), Data);
	}
}

//
// Talking to the bot
//

void ARoomStage::Send(const FString& Type, const TSharedRef<FJsonObject>& Data)
{
	if (Voice && bReady)
	{
		Voice->SendClientMessage(Type, ToJson(Data));
	}
}

void ARoomStage::SendWorld()
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Areas;
	if (House.IsValid())
	{
		for (const FRoomArea& Area : House->GetAreas())
		{
			TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
			One->SetStringField(TEXT("id"), Area.Id.ToString());
			One->SetStringField(TEXT("name"), Area.Name);
			Areas.Add(MakeShared<FJsonValueObject>(One));
		}
	}
	Data->SetArrayField(TEXT("areas"), Areas);
	Send(TEXT("world"), Data);
}

void ARoomStage::Say(const FString& Text)
{
	if (Text.IsEmpty() || !Voice)
	{
		return;
	}
	// Who'd hear it, said as loud as usual, here.
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	Heard.Reset();
	for (ARoomCharacter* Character : Characters)
	{
		if (Player && Reaches(PlayerHead(), Character->GetHeadLocation(), NormalRange, Player, Character))
		{
			Heard.Add(Character->GetId());
		}
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("heard"), Strings(Heard.Array()));
	Data->SetNumberField(TEXT("range"), NormalRange);
	LastEarshot = ToJson(Data);
	Send(TEXT("earshot"), Data);
	UE_LOG(LogRoomStage, Log, TEXT("Saying \"%s\", heard by %s"), *Text, *FString::Join(Heard.Array(), TEXT(", ")));
	Voice->SendText(Text);
	if (Captions)
	{
		Captions->SetLine(TEXT("user"), TEXT("You"), UserColor, Text);
		Captions->FadeOut(TEXT("user"), 4.0f);
	}
}

void ARoomStage::Pretend(float Seconds, float Loudness)
{
	PretendLeft = Seconds;
	PretendLoudness = Loudness;
}

void ARoomStage::HandleBotReady()
{
	bReady = true;
	LastSpace.Reset();
	LastEarshot.Reset();
	SendWorld();
	UE_LOG(LogRoomStage, Log, TEXT("The bot is ready"));
	if (Captions && bShowStatus)
	{
		Captions->SetStatus(TEXT("Connected"), 2.0f);
	}
	if (!FirstMessage.IsEmpty())
	{
		Say(FirstMessage);
	}
}

void ARoomStage::HandleDisconnected()
{
	bReady = false;
	UE_LOG(LogRoomStage, Log, TEXT("Disconnected from the bot"));
	if (Captions && bShowStatus)
	{
		Captions->SetStatus(TEXT("Disconnected"));
	}
}

void ARoomStage::HandleError(const FString& Error)
{
	UE_LOG(LogRoomStage, Warning, TEXT("%s"), *Error);
	// Not reaching the bot at all is worth saying, e.g. if it isn't running.
	if (Captions && (bShowStatus || !bReady))
	{
		Captions->SetStatus(bReady ? Error : FString::Printf(TEXT("Can't reach the bot at %s: is it running?"), *Voice->StartUrl), 10.0f);
	}
}

void ARoomStage::HandleUserTranscript(const FString& Text, bool bFinal)
{
	if (!Captions || Text.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	Captions->SetLine(TEXT("user"), TEXT("You"), UserColor, Text);
	if (bFinal)
	{
		Captions->FadeOut(TEXT("user"), 4.0f);
	}
}

void ARoomStage::HandleServerMessage(const FString& Message)
{
	HandleMessage(Message);
}

ARoomCharacter* ARoomStage::FindCharacter(const FString& Id) const
{
	for (const TObjectPtr<ARoomCharacter>& Character : Characters)
	{
		if (Character->GetId() == Id)
		{
			return Character;
		}
	}
	return nullptr;
}

AActor* ARoomStage::Resolve(const FString& Id) const
{
	if (Id == TEXT("user"))
	{
		return UGameplayStatics::GetPlayerPawn(this, 0);
	}
	return FindCharacter(Id);
}

float ARoomStage::Clarity(const ARoomCharacter* Character) const
{
	// Faint, and then gone, as they get further away, sooner through walls.
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	float Distance = FVector::Dist(PlayerHead(), Character->GetHeadLocation());
	if (!Reaches(Character->GetHeadLocation(), PlayerHead(), Distance + 1.0f, Character, Player))
	{
		Distance /= FMath::Max(ThroughWalls, 0.1f);
	}
	return 1.0f - FMath::SmoothStep(CharacterRange * 1.3f, CharacterRange * 2.6f, Distance);
}

FString ARoomStage::Label(const ARoomCharacter* Character) const
{
	// Only once the player has heard their name.
	return Known.Contains(Character->GetId()) ? Character->GetCast().Name : TEXT("?");
}

void ARoomStage::Learn(const FString& Text)
{
	for (const FRoomCast& Member : Members)
	{
		int32 At = Text.Find(Member.Name, ESearchCase::IgnoreCase);
		while (At != INDEX_NONE)
		{
			const bool bStart = At == 0 || !FChar::IsAlpha(Text[At - 1]);
			const int32 End = At + Member.Name.Len();
			const bool bEnd = End >= Text.Len() || !FChar::IsAlpha(Text[End]);
			if (bStart && bEnd)
			{
				Known.Add(Member.Id);
				break;
			}
			At = Text.Find(Member.Name, ESearchCase::IgnoreCase, ESearchDir::FromStart, At + 1);
		}
	}
}

void ARoomStage::HandleMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message), Json) || !Json)
	{
		return;
	}
	const FString Type = Json->GetStringField(TEXT("type"));

	if (Type == TEXT("line"))
	{
		// What someone says, as it starts playing, and again if it's cut short
		// or taken back.
		FString Id, Speaker, Text;
		Json->TryGetStringField(TEXT("id"), Id);
		Json->TryGetStringField(TEXT("speaker"), Speaker);
		Json->TryGetStringField(TEXT("text"), Text);
		bool bRemoved = false;
		Json->TryGetBoolField(TEXT("removed"), bRemoved);
		if (Speaker.IsEmpty())
		{
			if (const FString* Was = Lines.Find(Id))
			{
				Speaker = *Was;
			}
		}
		if (!Captions || Speaker.IsEmpty())
		{
			return;
		}
		if (bRemoved)
		{
			Captions->FadeOut(Speaker, 0.3f);
			return;
		}
		Lines.Add(Id, Speaker);
		if (Speaker == TEXT("user"))
		{
			Captions->SetLine(TEXT("user"), TEXT("You"), UserColor, Text);
			Captions->FadeOut(TEXT("user"), 4.0f);
			return;
		}
		if (ARoomCharacter* Character = FindCharacter(Speaker))
		{
			const float Clear = Clarity(Character);
			if (Clear > 0.3f)
			{
				Learn(Text);
			}
			Captions->SetLine(Speaker, Label(Character), Character->GetCast().Color, Text, Clear);
			if (!Speaking.Contains(Speaker))
			{
				Captions->FadeOut(Speaker, CaptionHold + 3.0f);
			}
		}
	}
	else if (Type == TEXT("voices"))
	{
		// Whose voices are playing.
		TSet<FString> Now(StringList(Json, TEXT("speakers")));
		for (const FString& Was : Speaking)
		{
			if (!Now.Contains(Was) && Captions)
			{
				Captions->FadeOut(Was, CaptionHold);
			}
			if (!Now.Contains(Was))
			{
				if (ARoomCharacter* Character = FindCharacter(Was))
				{
					Character->TalkTo(nullptr);
				}
			}
		}
		for (const FString& Id : Now)
		{
			if (ARoomCharacter* Character = FindCharacter(Id))
			{
				Character->SetThinking(false);
			}
		}
		Speaking = Now;
	}
	else if (Type == TEXT("turn"))
	{
		// Someone's line is being written: they're about to speak, to someone.
		FString Speaker, Target;
		Json->TryGetStringField(TEXT("speaker"), Speaker);
		Json->TryGetStringField(TEXT("target"), Target);
		if (ARoomCharacter* Character = FindCharacter(Speaker))
		{
			Character->SetThinking(true);
			if (!Target.IsEmpty())
			{
				Character->TalkTo(Resolve(Target));
			}
		}
	}
	else if (Type == TEXT("emote"))
	{
		FString MoodName, GestureName, Target, Part;
		Json->TryGetStringField(TEXT("mood"), MoodName);
		Json->TryGetStringField(TEXT("gesture"), GestureName);
		Json->TryGetStringField(TEXT("target"), Target);
		Json->TryGetStringField(TEXT("role"), Part);
		double Strength = 0.8;
		Json->TryGetNumberField(TEXT("strength"), Strength);
		double Seconds = Part == TEXT("listen") ? ListeningMoodSeconds : SpeakingMoodSeconds;
		Json->TryGetNumberField(TEXT("seconds"), Seconds);
		const ERoomMood Mood = RoomTypes::MoodFromName(MoodName);
		const ERoomGesture Gesture = RoomTypes::GestureFromName(GestureName);
		AActor* Toward = Target.IsEmpty() ? nullptr : Resolve(Target);
		for (const FString& Id : StringList(Json, TEXT("who")))
		{
			ARoomCharacter* Character = FindCharacter(Id);
			if (!Character)
			{
				continue;
			}
			if (!MoodName.IsEmpty())
			{
				Character->SetMood(Mood, static_cast<float>(Strength), static_cast<float>(Seconds));
			}
			if (Toward && Toward != Character)
			{
				if (Part == TEXT("speak"))
				{
					Character->TalkTo(Toward);
				}
				else
				{
					Character->LookAt(Toward, static_cast<float>(Seconds));
				}
			}
			Character->Gesture(Gesture, static_cast<float>(Strength));
			UE_LOG(LogRoomStage, Log, TEXT("%s feels %s (%.2f)%s%s"), *Id, RoomTypes::MoodName(Mood), Strength,
				Gesture != ERoomGesture::None ? TEXT(", ") : TEXT(""), Gesture != ERoomGesture::None ? RoomTypes::GestureName(Gesture) : TEXT(""));
		}
	}
	else if (Type == TEXT("act"))
	{
		FString What;
		Json->TryGetStringField(TEXT("action"), What);
		for (const FString& Id : StringList(Json, TEXT("who")))
		{
			if (ARoomCharacter* Character = FindCharacter(Id))
			{
				Act(Character, What);
			}
		}
	}
	else if (Type == TEXT("move"))
	{
		FString Order, Area;
		Json->TryGetStringField(TEXT("action"), Order);
		Json->TryGetStringField(TEXT("area"), Area);
		TArray<ARoomCharacter*> Who;
		for (const FString& Id : StringList(Json, TEXT("who")))
		{
			if (ARoomCharacter* Character = FindCharacter(Id))
			{
				Who.Add(Character);
			}
		}
		Move(Who, Order, Area.IsEmpty() ? NAME_None : FName(*Area));
	}
}

void ARoomStage::Move(const TArray<ARoomCharacter*>& Who, const FString& Order, FName Area)
{
	if (Who.IsEmpty())
	{
		return;
	}
	if (Order == TEXT("follow"))
	{
		for (ARoomCharacter* Character : Who)
		{
			Character->SetIntent(ERoomIntent::Follow);
		}
		// Everyone following has a place behind the player.
		TArray<ARoomCharacter*> Following;
		for (ARoomCharacter* Character : Characters)
		{
			if (Character->GetIntent() == ERoomIntent::Follow)
			{
				Following.Add(Character);
			}
		}
		for (int32 I = 0; I < Following.Num(); ++I)
		{
			Following[I]->SetSlot(I, Following.Num());
		}
	}
	else if (Order == TEXT("come"))
	{
		for (int32 I = 0; I < Who.Num(); ++I)
		{
			Who[I]->SetSlot(I, Who.Num());
			Who[I]->SetIntent(ERoomIntent::Come);
		}
	}
	else if (Order == TEXT("wait"))
	{
		for (ARoomCharacter* Character : Who)
		{
			Character->SetIntent(ERoomIntent::Wait);
		}
	}
	else if (Order == TEXT("home"))
	{
		for (ARoomCharacter* Character : Who)
		{
			Character->SetIntent(ERoomIntent::Home);
		}
	}
	else if (Order == TEXT("go") && !Area.IsNone())
	{
		for (int32 I = 0; I < Who.Num(); ++I)
		{
			Who[I]->SetSlot(I, Who.Num());
			Who[I]->SetIntent(ERoomIntent::Go, Area);
		}
	}
}
