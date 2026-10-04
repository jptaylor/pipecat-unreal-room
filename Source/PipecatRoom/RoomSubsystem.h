//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Subsystems/WorldSubsystem.h"

#include "RoomSubsystem.generated.h"

class ARoomHouse;
class ARoomStage;
class SWidget;
class UMediaPlayer;
class UMediaTexture;

// Builds the house when play begins, puts the player in it, and starts the
// conversation, with Pipecat's logo over the first moments. It also sets the
// game up to look its best: Epic quality, DLSS and its Frame Generation, and
// HDR, where the GPU and the display have them.
UCLASS(Config = Game)
class URoomSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** The bot's start endpoint. -PipecatStartUrl=... on the command line overrides it. */
	UPROPERTY(Config)
	FString StartUrl;

	/** How many frames per second the game runs at, at most, in step with the display. 0 for no limit. */
	UPROPERTY(Config)
	float FrameRateLimit = 60.0f;

	/** Whether to show as many frames again with DLSS Frame Generation, where the GPU has it. */
	UPROPERTY(Config)
	bool bFrameGeneration = true;

	/** Whether to show the game in HDR, where the display and Windows have it on. */
	UPROPERTY(Config)
	bool bHDR = true;

	/** How bright the HDR display is at its brightest, in nits. */
	UPROPERTY(Config)
	int32 HDRNits = 1000;

	/** Whether to show the engine's warnings on screen, e.g. about memory or DLSS, and how the connection is going. */
	UPROPERTY(Config)
	bool bDebugMessages = false;

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;

private:
	// Has DLSS render at a fixed resolution for the display's, where the GPU
	// has it, again when the window's resized.
	void StartDLSS();
	FDelegateHandle ResizedHandle;
	// Turns DLSS Frame Generation on, where the GPU has it. Whether it did.
	bool StartFrameGeneration();
	bool bFrameGenerationVSync = false;

	// Puts the player at the house's door, once they're there, with the
	// camera over their shoulder.
	void PlacePlayer();
	FTimerHandle PlaceTimer;
	int32 PlaceTries = 0;

	void StartStage();

	// Plays Pipecat's logo over the game, fading it out once it ends.
	void PlayLogo(UWorld& World);
	UFUNCTION()
	void HandleLogoEnded();
	UFUNCTION()
	void HandleLogoFailed(FString Url);

	TWeakObjectPtr<ARoomHouse> House;
	TWeakObjectPtr<ARoomStage> Stage;

	UPROPERTY()
	TObjectPtr<UMediaPlayer> LogoPlayer;

	UPROPERTY()
	TObjectPtr<UMediaTexture> LogoTexture;

	FSlateBrush LogoBrush;
	TSharedPtr<SWidget> Logo;
};
