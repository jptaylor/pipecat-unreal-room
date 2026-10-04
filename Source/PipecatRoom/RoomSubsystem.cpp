//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomSubsystem.h"

#include "RoomHouse.h"
#include "RoomStage.h"

#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"

#if WITH_DLSS
#include "DLSSLibrary.h"
#endif
#if WITH_DLSS_FRAME_GENERATION
#include "StreamlineLibraryDLSSG.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRoom, Log, All);

namespace
{
// Pipecat's logo: its size, and its background, which is also around it.
const FVector2D LogoSize(1920.0f, 1080.0f);
const FLinearColor LogoBackground = FLinearColor::FromSRGBColor(FColor(253, 253, 253));
// How long it fades out for, in seconds.
const float LogoFadeSeconds = 1.2f;

// For looking around without walking, e.g. Room.Teleport kitchen, or
// Room.Teleport 300 -200 90.
FAutoConsoleCommandWithWorldAndArgs TeleportCommand(
	TEXT("Room.Teleport"),
	TEXT("Puts the player in an area, e.g. Room.Teleport kitchen, or at a place, facing a way, e.g. Room.Teleport 300 -200 90"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
		ACharacter* Player = World ? Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
		if (!Player || Args.IsEmpty())
		{
			return;
		}
		FVector Location = Player->GetActorLocation();
		float Yaw = Player->GetActorRotation().Yaw;
		if (Args.Num() >= 2 && Args[0].IsNumeric())
		{
			Location = FVector(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), 100.0f);
			Yaw = Args.Num() > 2 ? FCString::Atof(*Args[2]) : Yaw;
		}
		else
		{
			for (TActorIterator<ARoomHouse> It(World); It; ++It)
			{
				if (const FRoomArea* Area = It->FindArea(FName(*Args[0])))
				{
					Location = FVector(Area->Hub, 100.0f);
				}
			}
		}
		Player->SetActorLocationAndRotation(Location, FRotator(0.0f, Yaw, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Controller = Player->GetController())
		{
			Controller->SetControlRotation(FRotator(-10.0f, Yaw, 0.0f));
		}
	}));
} // namespace

bool URoomSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void URoomSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);

	// The engine's Epic quality, with the project's own settings on top, is
	// what the game is tuned for, whatever was saved before, but for the
	// resolution, which is left to the renderer. And a steady frame rate, in
	// step with the display, is smoother than as many frames as possible.
	//
	// Where the GPU has DLSS Frame Generation, every other frame shown is
	// generated, and Reflex paces them, with the display's VSync only where
	// Frame Generation supports it. Where the display has HDR, the game is
	// shown in it.
	if (UGameUserSettings* Settings = GEngine->GetGameUserSettings())
	{
		const int32 Epic = 3;
		Settings->SetViewDistanceQuality(Epic);
		Settings->SetAntiAliasingQuality(Epic);
		Settings->SetShadowQuality(Epic);
		Settings->SetGlobalIlluminationQuality(Epic);
		Settings->SetReflectionQuality(Epic);
		Settings->SetPostProcessingQuality(Epic);
		Settings->SetTextureQuality(Epic);
		Settings->SetVisualEffectQuality(Epic);
		Settings->SetFoliageQuality(Epic);
		Settings->SetShadingQuality(Epic);
		Settings->SetLandscapeQuality(Epic);
		StartDLSS();
		const bool bGenerating = StartFrameGeneration();
		if (FrameRateLimit > 0.0f)
		{
			Settings->SetFrameRateLimit(bGenerating ? 2.0f * FrameRateLimit : FrameRateLimit);
			Settings->SetVSyncEnabled(!bGenerating || bFrameGenerationVSync);
		}
		Settings->EnableHDRDisplayOutput(bHDR, HDRNits);
		Settings->ApplyNonResolutionSettings();

		// In HDR, the engine blends the UI so that the frame's alpha doesn't
		// tell Frame Generation where the UI is, and only gets in its way.
		if (IConsoleVariable* TagUI = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streamline.TagUIColorAlpha")))
		{
			TagUI->Set(Settings->IsHDREnabled() ? 0 : 1, ECVF_SetByCode);
		}
		UE_LOG(LogRoom, Log, TEXT("Frame Generation %s, HDR %s"),
			bGenerating ? TEXT("on") : TEXT("off"), Settings->IsHDREnabled() ? TEXT("on") : TEXT("off"));
	}

	// Nothing on screen but the game, e.g. to record it, unless asked for.
	if (!bDebugMessages && GEngine)
	{
		GEngine->bEnableOnScreenDebugMessages = false;
		GEngine->Exec(&World, TEXT("DisableAllScreenMessages"));
	}

	House = World.SpawnActor<ARoomHouse>();
	PlacePlayer();
	StartStage();
	if (!FParse::Param(FCommandLine::Get(), TEXT("PipecatNoLogo")))
	{
		PlayLogo(World);
	}
}

void URoomSubsystem::PlacePlayer()
{
	UWorld* World = GetWorld();
	ACharacter* Player = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(World, 0));
	if (!Player)
	{
		// Not there yet: again in a moment.
		if (++PlaceTries < 100)
		{
			World->GetTimerManager().SetTimer(PlaceTimer, FTimerDelegate::CreateUObject(this, &URoomSubsystem::PlacePlayer), 0.05f, false);
		}
		return;
	}
	if (House.IsValid())
	{
		const FRoomSpot Start = House->GetPlayerStart();
		Player->SetActorLocationAndRotation(Start.Location + FVector(0.0f, 0.0f, 100.0f), FRotator(0.0f, Start.Yaw, 0.0f), false,
			nullptr, ETeleportType::TeleportPhysics);
		if (AController* Controller = Player->GetController())
		{
			Controller->SetControlRotation(FRotator(-8.0f, Start.Yaw, 0.0f));
		}
	}
	// The camera close over their shoulder, easing after them, indoors.
	if (USpringArmComponent* Arm = Player->FindComponentByClass<USpringArmComponent>())
	{
		Arm->TargetArmLength = 320.0f;
		Arm->SocketOffset = FVector(0.0f, 45.0f, 35.0f);
		Arm->bEnableCameraLag = true;
		Arm->CameraLagSpeed = 10.0f;
		Arm->ProbeSize = 14.0f;
	}
	if (UCameraComponent* Camera = Player->FindComponentByClass<UCameraComponent>())
	{
		Camera->SetFieldOfView(80.0f);
	}
	// Walking, not running, indoors.
	if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = 380.0f;
	}
}

void URoomSubsystem::StartStage()
{
	UWorld* World = GetWorld();
	FString Url = StartUrl;
	FParse::Value(FCommandLine::Get(), TEXT("PipecatStartUrl="), Url);
	// E.g. a Pipecat Cloud public API key.
	FString ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("PIPECAT_API_KEY"));
	FParse::Value(FCommandLine::Get(), TEXT("PipecatApiKey="), ApiKey);

	ARoomStage* NewStage = World->SpawnActorDeferred<ARoomStage>(ARoomStage::StaticClass(), FTransform::Identity);
	if (!NewStage)
	{
		UE_LOG(LogRoom, Error, TEXT("Unable to start the conversation"));
		return;
	}
	NewStage->StartUrl = Url;
	NewStage->ApiKey = ApiKey;
	// For trying it without talking, e.g. -PipecatNoMicrophone -PipecatSay="Hello?".
	NewStage->bUseMicrophone = !FParse::Param(FCommandLine::Get(), TEXT("PipecatNoMicrophone"));
	FParse::Value(FCommandLine::Get(), TEXT("PipecatSay="), NewStage->FirstMessage);
	// E.g. -PipecatVolume=0 to try it silently.
	FParse::Value(FCommandLine::Get(), TEXT("PipecatVolume="), NewStage->VoiceVolume);
	NewStage->bShowStatus = bDebugMessages;
	NewStage->FinishSpawning(FTransform::Identity);
	Stage = NewStage;
}

void URoomSubsystem::StartDLSS()
{
#if WITH_DLSS
	if (!UDLSSLibrary::IsDLSSSupported())
	{
		return;
	}
	// DLSS renders at a fixed resolution for the display's, as NVIDIA has it:
	// Unreal's dynamic resolution would have it switch between DLAA and
	// scaling up, which fails. Performance at 4K, Quality at 1440p or 1080p,
	// and DLAA in a smaller window.
	if (!ResizedHandle.IsValid())
	{
		ResizedHandle = FViewport::ViewportResizedEvent.AddWeakLambda(this, [this](FViewport*, uint32) { StartDLSS(); });
	}
	// Until the window has its size, when this is called again.
	const FIntPoint Size = GEngine->GameViewport && GEngine->GameViewport->Viewport
		? GEngine->GameViewport->Viewport->GetSizeXY()
		: FIntPoint::ZeroValue;
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return;
	}
	const int64 Pixels = int64(Size.X) * Size.Y;
	const UDLSSMode Mode = Pixels >= 3'690'000 ? UDLSSMode::Performance : (Pixels >= 2'030'000 ? UDLSSMode::Quality : UDLSSMode::DLAA);
	bool bSupported = false;
	bool bFixed = false;
	float ScreenPercentage = 0.0f;
	float MinScreenPercentage = 0.0f;
	float MaxScreenPercentage = 0.0f;
	float Sharpness = 0.0f;
	UDLSSLibrary::GetDLSSModeInformation(Mode, FVector2D(Size), bSupported, ScreenPercentage, bFixed, MinScreenPercentage, MaxScreenPercentage, Sharpness);
	if (!bSupported || ScreenPercentage <= 0.0f)
	{
		ScreenPercentage = 100.0f;
	}
	IConsoleManager& Console = IConsoleManager::Get();
	if (IConsoleVariable* DynamicResolution = Console.FindConsoleVariable(TEXT("r.DynamicRes.OperationMode")))
	{
		DynamicResolution->Set(0, ECVF_SetByCode);
	}
	if (IConsoleVariable* Percentage = Console.FindConsoleVariable(TEXT("r.ScreenPercentage")))
	{
		Percentage->Set(ScreenPercentage, ECVF_SetByCode);
	}
	UE_LOG(LogRoom, Log, TEXT("DLSS at %.0f%% of %dx%d"), ScreenPercentage, Size.X, Size.Y);
#endif
}

bool URoomSubsystem::StartFrameGeneration()
{
	bFrameGenerationVSync = false;
#if WITH_DLSS_FRAME_GENERATION
	if (bFrameGeneration && UStreamlineLibraryDLSSG::IsDLSSGSupported())
	{
		// Auto turns it off wherever it would cost frames rather than add them.
		UStreamlineLibraryDLSSG::SetDLSSGMode(EStreamlineDLSSGMode::Auto);
		bFrameGenerationVSync = UStreamlineLibraryDLSSG::GetDLSSGIsVsyncSupportAvailable();
		return true;
	}
#endif
	return false;
}

void URoomSubsystem::PlayLogo(UWorld& World)
{
	UGameViewportClient* Viewport = World.GetGameViewport();
	if (!Viewport)
	{
		return;
	}

	LogoPlayer = NewObject<UMediaPlayer>(this);
	LogoPlayer->OnEndReached.AddDynamic(this, &URoomSubsystem::HandleLogoEnded);
	LogoPlayer->OnMediaOpenFailed.AddDynamic(this, &URoomSubsystem::HandleLogoFailed);

	LogoTexture = NewObject<UMediaTexture>(this);
	LogoTexture->ClearColor = LogoBackground;
	LogoTexture->SetDefaultMediaPlayer(LogoPlayer);
	LogoTexture->UpdateResource();

	LogoBrush.SetResourceObject(LogoTexture);
	LogoBrush.ImageSize = LogoSize;
	Logo = SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(LogoBackground)
		.Padding(0.0f)
		[
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			[
				SNew(SImage).Image(&LogoBrush)
			]
		];
	Viewport->AddViewportWidgetContent(Logo.ToSharedRef(), 100);

	// Unreal plays MP4 on Windows and macOS, but on Linux only WebM.
	FString Path = FPaths::ProjectContentDir() / (PLATFORM_LINUX ? TEXT("Movies/pipecat.webm") : TEXT("Movies/pipecat.mp4"));
	if (!LogoPlayer->OpenFile(Path))
	{
		HandleLogoFailed(Path);
	}
}

void URoomSubsystem::HandleLogoEnded()
{
	if (!Logo)
	{
		return;
	}
	TSharedPtr<SWidget> Fading = Logo;
	Logo.Reset();
	TWeakObjectPtr<URoomSubsystem> Self(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Self, Fading, Elapsed = 0.0f](float DeltaSeconds) mutable
		{
			Elapsed += DeltaSeconds;
			if (Self.IsValid() && Elapsed < LogoFadeSeconds)
			{
				Fading->SetRenderOpacity(1.0f - FMath::SmoothStep(0.0f, 1.0f, Elapsed / LogoFadeSeconds));
				return true;
			}
			if (Self.IsValid())
			{
				if (UGameViewportClient* Viewport = Self->GetWorld() ? Self->GetWorld()->GetGameViewport() : nullptr)
				{
					Viewport->RemoveViewportWidgetContent(Fading.ToSharedRef());
				}
				if (Self->LogoPlayer)
				{
					Self->LogoPlayer->Close();
				}
			}
			return false;
		}));
}

void URoomSubsystem::HandleLogoFailed(FString Url)
{
	UE_LOG(LogRoom, Warning, TEXT("Unable to play %s"), *Url);
	HandleLogoEnded();
}
