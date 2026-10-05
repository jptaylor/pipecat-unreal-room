//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "RoomHouse.generated.h"

class UDirectionalLightComponent;
class ULightComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class USpotLightComponent;
class UStaticMesh;
class UStaticMeshComponent;

// A room of the house, or part of it: where it is, what it's called (the bot
// says it, e.g. "the kitchen"), and the middle of its floor, which paths
// through it go by.
struct FRoomArea
{
	FName Id;
	FString Name;
	FBox2D Box;
	float Ceiling = 450.0f;
	FVector2D Hub;
};

// A doorway between two areas, in a wall along X or along Y.
struct FRoomDoor
{
	FName From;
	FName To;
	FVector2D Center;
	// Whether the wall it's in runs along X (so it's walked through along Y).
	bool bWallAlongX = false;
	float Width = 200.0f;
};

// Where a character stands at home, and which way they face, in degrees.
struct FRoomSpot
{
	FVector Location;
	float Yaw = 0.0f;
};

// Something in the house the player can look at: what it's called, e.g. "the
// gramophone" or "a sofa", where its middle is, and about how big it is.
struct FRoomSight
{
	FString Name;
	FVector Center = FVector::ZeroVector;
	float Radius = 50.0f;
};

// The house: rooms with doorways between them, windows and skylights, plain
// furniture, all flat colors, lit by the sun and the sky through the windows,
// and by lamps that swing and sweep, all casting shadows. It also finds the
// way from one room to another for the characters.
UCLASS(Config = Game)
class ARoomHouse : public AActor
{
	GENERATED_BODY()

public:
	ARoomHouse();

	/** How long the sun takes to drift across the sky and back, in seconds. */
	UPROPERTY(Config)
	float SunDriftSeconds = 360.0f;

	virtual void Tick(float DeltaSeconds) override;

	const TArray<FRoomArea>& GetAreas() const { return Areas; }
	const FRoomArea* FindArea(FName Id) const;
	/** The area `Location` is in, or None outside. */
	FName AreaAt(const FVector& Location) const;

	/** Where the player starts, facing which way. */
	FRoomSpot GetPlayerStart() const;

	/** Where a character lives in an area, e.g. "kitchen". */
	FRoomSpot GetHome(FName Area) const;

	/**
	 * A way from `From` to `To` through the doorways and around the furniture,
	 * ending at `To`, and around whatever else is in the way, e.g. people
	 * standing about.
	 */
	void FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPath, TConstArrayView<FBox2D> Avoid = {}) const;

	/** How far it is to walk that way. */
	float PathLength(const FVector& From, const FVector& To) const;

	/** The furniture of `Actor`, e.g. the things in the house, for the characters to walk around. */
	void AddBlockers(const AActor* Actor);

	/** `Location`, or if it's in the furniture, just clear of it. */
	FVector ClearOf(const FVector& Location) const;

	/**
	 * A clear spot `Distance` from `Target`, in the same room, that's the
	 * shortest walk from `From`, e.g. to hand someone something.
	 */
	FVector FindSpotBy(const FVector& Target, const FVector& From, float Distance) const;

	/** The nearest point to `Location` that's inside an area, well away from its walls. */
	FVector KeepInside(const FVector& Location) const;

	/** The conservatory's trees, by planter, grow a little each time they're watered. */
	void Grow(int32 Planter);

	/** The gallery's sculptures, which the player can spin. */
	int32 GetSculptureCount() const { return Sculptures.Num(); }
	FVector GetSculptureLocation(int32 Index) const;
	void SpinSculpture(int32 Index, float Degrees);

	/** What the player can look at in the house, built into it: its furniture, trees, sculptures and paintings. */
	const TArray<FRoomSight>& GetSights() const { return Sights; }
	void AddSight(const FString& Name, const FVector& Center, float Radius) { Sights.Add({Name, Center, Radius}); }

	/** The hall's lamps, as party lights: how much (0 to 1), on what beat, and how loud the music is. */
	void SetParty(float Amount, double Beat, float Loudness);

protected:
	virtual void BeginPlay() override;

private:
	// Each piece of the house, as it's built.
	UStaticMeshComponent* AddMesh(UStaticMesh* Mesh, const FTransform& Transform, const FLinearColor& Color,
		float Roughness = 0.65f, bool bCollide = true);
	UStaticMeshComponent* AddBox(const FVector& Min, const FVector& Max, const FLinearColor& Color,
		float Roughness = 0.65f, bool bCollide = true);
	UStaticMeshComponent* AddCylinder(const FVector& Base, float Radius, float Height, const FLinearColor& Color,
		float Roughness = 0.65f, bool bCollide = true);
	UStaticMeshComponent* AddSphere(const FVector& Center, const FVector& Radii, const FLinearColor& Color,
		float Roughness = 0.65f, bool bCollide = true);
	UStaticMeshComponent* AddLamp(const FVector& Center, float Radius, const FLinearColor& Color, float Glow);
	// The camera stays out of it, though people don't bump into it.
	UStaticMeshComponent* BlockCamera(UStaticMeshComponent* Component);

	// A wall along X (bAlongX) or along Y, from A to B, with openings, each
	// along it (from its start), how wide, and from and to how high.
	struct FOpening
	{
		float At;
		float Width;
		float Bottom;
		float Top;
	};
	void AddWall(const FVector2D& A, const FVector2D& B, float Height, const TArray<FOpening>& Openings,
		const FLinearColor& Color, float Thickness = 40.0f);
	// How the walls being built look below a rail at Split cm (none at 0), and
	// their skirting boards, rails and door and window frames.
	struct FWallStyle
	{
		FLinearColor Lower = FLinearColor::White;
		float Split = 0.0f;
		FLinearColor Trim = FLinearColor::White;
		FLinearColor Frame = FLinearColor::White;
	};
	FWallStyle Style;
	// A framed picture on a wall, facing out from it, of colored blocks.
	void AddPicture(const FVector& Center, const FVector& Facing, float Width, float Height, const TArray<FLinearColor>& Colors);
	// A ceiling over an area, with holes (skylights).
	void AddCeiling(const FBox2D& Box, float Height, const TArray<FBox2D>& Holes, const FLinearColor& Color);

	UMaterialInstanceDynamic* GetMaterial(const FLinearColor& Color, float Roughness, float Glow, const FLinearColor& Emissive);

	UPointLightComponent* AddPointLight(const FVector& Location, const FLinearColor& Color, float Intensity, float Radius,
		float SourceRadius = 8.0f);
	USpotLightComponent* AddSpotLight(const FVector& Location, const FRotator& Rotation, const FLinearColor& Color,
		float Intensity, float Radius, float ConeAngle);

	void BuildSky();
	void BuildHall();
	void BuildConservatory();
	void BuildKitchen();
	void BuildMusicRoom();
	void BuildGallery();
	void BuildOutside();
	// Furnishings: sofas, lamps, plants, rugs and the like, in every room.
	void BuildDressing();
	UStaticMeshComponent* AddBlock(const FVector& Center, const FVector& Size, float Yaw, const FLinearColor& Color,
		float Roughness = 0.65f, bool bCollide = true);
	void AddSofa(const FVector& Center, float Yaw, float Width, const FLinearColor& Color, const FLinearColor& Cushion);
	void AddTableLamp(const FVector& Base, const FLinearColor& Shade, float Bright);
	void AddFloorLamp(const FVector& Base);
	void AddSideTable(const FVector& Base, float Size, float Height, const FLinearColor& Color);
	void AddPottedPlant(const FVector& Base, float Scale, const FLinearColor& Pot);
	void AddRug(const FVector& Center, const FVector2D& Size, float Yaw, const FLinearColor& Color, const FLinearColor& Border);
	void AddSconce(const FVector& At, const FVector& Facing);
	void AddFruitBowl(const FVector& At);
	void AddBooks(const FVector& At, float Yaw, int32 Count, int32 Seed);
	void AddStringLights(const FVector& From, const FVector& To, float Sag, int32 Count);

	// The way through the doorways alone, and around the furniture in a room.
	void FindDoorways(const FVector& From, const FVector& To, TArray<FVector>& OutPath) const;
	void FindAround(const FVector2D& From, const FVector2D& To, FName Area, TConstArrayView<FBox2D> Avoid, TArray<FVector2D>& OutPath) const;
	void AddBlocker(const FBox& Bounds);

	void AddArea(FName Id, const FString& Name, const FBox2D& Box, float Ceiling, const FVector2D& Hub);
	void AddDoor(FName From, FName To, const FVector2D& Center, bool bWallAlongX, float Width);

	UPROPERTY()
	TObjectPtr<UStaticMesh> Cube;

	UPROPERTY()
	TObjectPtr<UStaticMesh> Cylinder;

	UPROPERTY()
	TObjectPtr<UStaticMesh> Sphere;

	UPROPERTY()
	TObjectPtr<UStaticMesh> Cone;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> Flat;

	UPROPERTY()
	TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Sun;

	// Lamps that swing from where they hang: the pivot, the lamp, and how.
	struct FSwinging
	{
		TObjectPtr<USceneComponent> Pivot;
		float Amplitude;
		float Period;
		float Phase;
	};
	TArray<FSwinging> Swinging;

	// Spotlights that sweep from side to side about where they're aimed.
	struct FSweeping
	{
		TObjectPtr<USpotLightComponent> Light;
		FRotator Aim;
		float Amplitude;
		float Period;
		float Phase;
	};
	TArray<FSweeping> Sweeping;

	// The hall's lamps, and their shades, which party with the music.
	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> HallLamps;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> HallShades;

	// The conservatory's trees' canopies, two a planter, and how big they're
	// getting, and are.
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Canopies;
	TArray<float> Growth;
	TArray<float> Grown;
	TArray<FVector> CanopySizes;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Sculptures;
	TArray<FRoomSight> Sights;

	float Party = 0.0f;
	float PartyTarget = 0.0f;
	double PartyBeat = 0.0;
	float PartyLoudness = 0.0f;

	TArray<FRoomArea> Areas;
	TArray<FRoomDoor> Doors;
	// The furniture, on the floor plan, which paths go around; walls aren't
	// furniture, as paths go through their doorways.
	TArray<FBox2D> Blockers;
	bool bBuildingWall = false;
	TMap<FName, FRoomSpot> Homes;
	FRotator SunAim;
	float Time = 0.0f;
};
