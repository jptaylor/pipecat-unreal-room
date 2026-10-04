//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomHouse.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoomHouse, Log, All);

namespace
{
// Created by setup.ps1.
const TCHAR* const FlatPath = TEXT("/Game/Room/M_Flat.M_Flat");
const TCHAR* const CloudsPath = TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst");

FLinearColor Srgb(uint8 R, uint8 G, uint8 B)
{
	return FLinearColor::FromSRGBColor(FColor(R, G, B));
}

FLinearColor Warm(float Kelvin)
{
	return FLinearColor::MakeFromColorTemperature(Kelvin);
}

// The house's colors.
const FLinearColor Plaster = Srgb(226, 224, 219);
const FLinearColor Ceiling = Srgb(234, 233, 230);
const FLinearColor Column = Srgb(238, 236, 232);
const FLinearColor Stone = Srgb(186, 182, 176);
const FLinearColor Skirting = Srgb(70, 66, 62);
const FLinearColor Charcoal = Srgb(58, 58, 62);
const FLinearColor Leaf = Srgb(92, 140, 84);
const FLinearColor LeafLight = Srgb(126, 166, 98);
const FLinearColor LeafDark = Srgb(70, 112, 72);
const FLinearColor Bark = Srgb(120, 92, 70);

// How thick the walls are, and how far from them paths keep.
const float Wall = 40.0f;
const float Margin = 70.0f;
// How far from furniture paths keep: a character's width, and a little more
// at its corners, which paths turn at.
const float Clearance = 48.0f;
const float CornerClearance = 72.0f;

// Whether the straight line from A to B goes through Box.
bool Crosses(const FVector2D& A, const FVector2D& B, const FBox2D& Box)
{
	const FVector2D D = B - A;
	double Enter = 0.0;
	double Leave = 1.0;
	auto Clip = [&](double P, double Q) {
		if (FMath::IsNearlyZero(P))
		{
			return Q > 0.0;
		}
		const double R = Q / P;
		if (P < 0.0)
		{
			Enter = FMath::Max(Enter, R);
		}
		else
		{
			Leave = FMath::Min(Leave, R);
		}
		return Enter < Leave;
	};
	return Clip(-D.X, A.X - Box.Min.X) && Clip(D.X, Box.Max.X - A.X) && Clip(-D.Y, A.Y - Box.Min.Y) && Clip(D.Y, Box.Max.Y - A.Y) &&
		   Leave - Enter > 1.0e-4;
}
} // namespace

ARoomHouse::ARoomHouse()
{
	PrimaryActorTick.bCanEverTick = true;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	RootComponent = Root;
}

void ARoomHouse::BeginPlay()
{
	Super::BeginPlay();

	Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	Flat = LoadObject<UMaterialInterface>(nullptr, FlatPath);
	if (!Flat)
	{
		UE_LOG(LogRoomHouse, Warning, TEXT("No %s: run setup.ps1. Using the engine's default material."), FlatPath);
	}

	// The rooms, and the doorways between them, which the characters find
	// their way through.
	AddArea(TEXT("hall"), TEXT("the hall"), FBox2D(FVector2D(-800.0f, -800.0f), FVector2D(800.0f, 800.0f)), 900.0f, FVector2D(0.0f, 0.0f));
	AddArea(TEXT("conservatory"), TEXT("the conservatory"), FBox2D(FVector2D(800.0f, -900.0f), FVector2D(2200.0f, 900.0f)), 700.0f, FVector2D(1150.0f, 0.0f));
	AddArea(TEXT("kitchen"), TEXT("the kitchen"), FBox2D(FVector2D(-600.0f, -2000.0f), FVector2D(600.0f, -800.0f)), 450.0f, FVector2D(0.0f, -1150.0f));
	AddArea(TEXT("music"), TEXT("the music room"), FBox2D(FVector2D(-600.0f, 800.0f), FVector2D(600.0f, 2000.0f)), 500.0f, FVector2D(0.0f, 1150.0f));
	AddArea(TEXT("gallery"), TEXT("the gallery"), FBox2D(FVector2D(-2200.0f, -700.0f), FVector2D(-800.0f, 700.0f)), 600.0f, FVector2D(-1200.0f, 0.0f));
	AddDoor(TEXT("hall"), TEXT("conservatory"), FVector2D(800.0f, 0.0f), false, 360.0f);
	AddDoor(TEXT("hall"), TEXT("kitchen"), FVector2D(0.0f, -800.0f), true, 220.0f);
	AddDoor(TEXT("hall"), TEXT("music"), FVector2D(0.0f, 800.0f), true, 220.0f);
	AddDoor(TEXT("hall"), TEXT("gallery"), FVector2D(-800.0f, 0.0f), false, 300.0f);

	// Where the characters live: Maya among the plants, Theo at the kitchen
	// island, Juno on the music room's stage.
	Homes.Add(TEXT("conservatory"), {FVector(1620.0f, -330.0f, 0.0f), 150.0f});
	Homes.Add(TEXT("kitchen"), {FVector(-300.0f, -1500.0f, 0.0f), 35.0f});
	Homes.Add(TEXT("music"), {FVector(120.0f, 1760.0f, 25.0f), -100.0f});
	Homes.Add(TEXT("hall"), {FVector(300.0f, 300.0f, 0.0f), 180.0f});
	Homes.Add(TEXT("gallery"), {FVector(-1450.0f, -250.0f, 0.0f), 0.0f});

	BuildSky();
	BuildHall();
	BuildConservatory();
	BuildKitchen();
	BuildMusicRoom();
	BuildGallery();
	BuildOutside();
}

void ARoomHouse::AddArea(FName Id, const FString& Name, const FBox2D& Box, float CeilingHeight, const FVector2D& Hub)
{
	FRoomArea Area;
	Area.Id = Id;
	Area.Name = Name;
	Area.Box = Box;
	Area.Ceiling = CeilingHeight;
	Area.Hub = Hub;
	Areas.Add(Area);
}

void ARoomHouse::AddDoor(FName From, FName To, const FVector2D& Center, bool bWallAlongX, float Width)
{
	FRoomDoor Door;
	Door.From = From;
	Door.To = To;
	Door.Center = Center;
	Door.bWallAlongX = bWallAlongX;
	Door.Width = Width;
	Doors.Add(Door);
}

//
// Building blocks
//

UMaterialInstanceDynamic* ARoomHouse::GetMaterial(const FLinearColor& Color, float Roughness, float Glow, const FLinearColor& Emissive)
{
	const FString Key = FString::Printf(TEXT("%s/%.2f/%.2f/%s"), *Color.ToString(), Roughness, Glow, *Emissive.ToString());
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Materials.Find(Key))
	{
		return *Found;
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(
		Flat ? Flat.Get() : UMaterial::GetDefaultMaterial(MD_Surface), this);
	Material->SetVectorParameterValue(TEXT("Color"), Color);
	Material->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	Material->SetVectorParameterValue(TEXT("Emissive"), Emissive);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), Glow);
	Materials.Add(Key, Material);
	return Material;
}

UStaticMeshComponent* ARoomHouse::AddMesh(
	UStaticMesh* Mesh, const FTransform& Transform, const FLinearColor& Color, float Roughness, bool bCollide)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetStaticMesh(Mesh);
	Component->SetupAttachment(RootComponent);
	Component->SetRelativeTransform(Transform);
	Component->SetMaterial(0, GetMaterial(Color, Roughness, 0.0f, FLinearColor::Black));
	Component->SetCollisionProfileName(bCollide ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	Component->RegisterComponent();
	if (bCollide && !bBuildingWall)
	{
		AddBlocker(Component->Bounds.GetBox());
	}
	return Component;
}

void ARoomHouse::AddBlocker(const FBox& Bounds)
{
	// What a character would walk into: on the floor, or low enough to bump
	// their head, and higher than a step.
	if (Bounds.Max.Z > 30.0f && Bounds.Min.Z < 120.0f)
	{
		Blockers.Add(FBox2D(FVector2D(Bounds.Min), FVector2D(Bounds.Max)));
	}
}

void ARoomHouse::AddBlockers(const AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	TArray<UPrimitiveComponent*> Parts;
	Actor->GetComponents(Parts);
	for (const UPrimitiveComponent* Part : Parts)
	{
		if (Part->IsRegistered() && Part->IsCollisionEnabled())
		{
			AddBlocker(Part->Bounds.GetBox());
		}
	}
	UE_LOG(LogRoomHouse, Log, TEXT("%d pieces of furniture to walk around"), Blockers.Num());
}

UStaticMeshComponent* ARoomHouse::AddBox(const FVector& Min, const FVector& Max, const FLinearColor& Color, float Roughness, bool bCollide)
{
	const FVector Size = (Max - Min).GetAbs();
	if (Size.X < 0.5f || Size.Y < 0.5f || Size.Z < 0.5f)
	{
		return nullptr;
	}
	return AddMesh(Cube, FTransform(FRotator::ZeroRotator, (Min + Max) * 0.5f, Size / 100.0f), Color, Roughness, bCollide);
}

UStaticMeshComponent* ARoomHouse::AddCylinder(
	const FVector& Base, float Radius, float Height, const FLinearColor& Color, float Roughness, bool bCollide)
{
	const FVector Scale(Radius / 50.0f, Radius / 50.0f, Height / 100.0f);
	return AddMesh(Cylinder, FTransform(FRotator::ZeroRotator, Base + FVector(0.0f, 0.0f, Height * 0.5f), Scale), Color, Roughness, bCollide);
}

UStaticMeshComponent* ARoomHouse::AddSphere(
	const FVector& Center, const FVector& Radii, const FLinearColor& Color, float Roughness, bool bCollide)
{
	return AddMesh(Sphere, FTransform(FRotator::ZeroRotator, Center, Radii / 50.0f), Color, Roughness, bCollide);
}

UStaticMeshComponent* ARoomHouse::AddLamp(const FVector& Center, float Radius, const FLinearColor& Color, float Glow)
{
	UStaticMeshComponent* Lamp = AddSphere(Center, FVector(Radius), Color, 0.4f, false);
	Lamp->SetMaterial(0, GetMaterial(Color, 0.4f, Glow, Color));
	// The light inside it shines out through it.
	Lamp->SetCastShadow(false);
	return Lamp;
}

void ARoomHouse::AddWall(
	const FVector2D& A, const FVector2D& B, float Height, const TArray<FOpening>& Openings, const FLinearColor& Color, float Thickness)
{
	TGuardValue<bool> Building(bBuildingWall, true);
	const bool bAlongX = FMath::IsNearlyEqual(A.Y, B.Y);
	const float Start = bAlongX ? FMath::Min(A.X, B.X) : FMath::Min(A.Y, B.Y);
	const float End = bAlongX ? FMath::Max(A.X, B.X) : FMath::Max(A.Y, B.Y);
	const float Across = bAlongX ? A.Y : A.X;
	// A box along the wall, from and to so far along it, and from and to so
	// high, as thick as the wall and `Proud` more.
	auto Slab = [&](float From, float To, float Bottom, float Top, const FLinearColor& SlabColor, float Proud, float Roughness) {
		if (To - From < 1.0f || Top - Bottom < 1.0f)
		{
			return;
		}
		const float Half = Thickness / 2 + Proud;
		if (bAlongX)
		{
			AddBox(FVector(From, Across - Half, Bottom), FVector(To, Across + Half, Top), SlabColor, Roughness);
		}
		else
		{
			AddBox(FVector(Across - Half, From, Bottom), FVector(Across + Half, To, Top), SlabColor, Roughness);
		}
	};
	// A piece of wall: in the room's two colors, split by a rail, with a
	// skirting board at the floor.
	auto Piece = [&](float From, float To, float Bottom, float Top) {
		const float Split = Style.Split;
		if (Split > Bottom && Split < Top)
		{
			Slab(From, To, Bottom, Split, Style.Lower, 0.0f, 0.6f);
			Slab(From, To, Split, Top, Color, 0.0f, 0.75f);
			Slab(From, To, Split - 3.0f, Split + 3.0f, Style.Trim, 2.0f, 0.4f);
		}
		else
		{
			Slab(From, To, Bottom, Top, Split >= Top ? Style.Lower : Color, 0.0f, 0.75f);
		}
		if (Bottom <= 0.0f && Top > 14.0f)
		{
			Slab(From, To, 0.0f, 13.0f, Style.Trim, 1.5f, 0.4f);
		}
	};
	TArray<FOpening> Sorted = Openings;
	Sorted.Sort([](const FOpening& L, const FOpening& R) { return L.At < R.At; });
	float Cursor = Start;
	for (const FOpening& Opening : Sorted)
	{
		const float Left = Start + Opening.At - Opening.Width / 2;
		const float Right = Start + Opening.At + Opening.Width / 2;
		Piece(Cursor, Left, 0.0f, Height);
		Piece(Left, Right, Opening.Top, Height);
		Piece(Left, Right, 0.0f, Opening.Bottom);
		Cursor = Right;
		// A frame around it, and a sill under a window.
		const float Frame = 9.0f;
		// Each a little into the opening, so its faces aren't the wall's.
		const float Lip = 1.5f;
		Slab(Left - Frame, Left + Lip, Opening.Bottom, Opening.Top + Frame, Style.Frame, 2.5f, 0.45f);
		Slab(Right - Lip, Right + Frame, Opening.Bottom, Opening.Top + Frame, Style.Frame, 2.5f, 0.45f);
		Slab(Left - Frame, Right + Frame, Opening.Top - Lip, Opening.Top + Frame, Style.Frame, 2.5f, 0.45f);
		if (Opening.Bottom > 0.0f)
		{
			Slab(Left - Frame, Right + Frame, Opening.Bottom - 5.0f, Opening.Bottom + Lip, Style.Frame, 7.0f, 0.45f);
		}
	}
	Piece(Cursor, End, 0.0f, Height);
}

void ARoomHouse::AddPicture(const FVector& Center, const FVector& Facing, float Width, float Height, const TArray<FLinearColor>& Colors)
{
	// A frame, proud of the wall, and a canvas of colored blocks, flat as the
	// rest of the house, in bands.
	const FVector Along = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
	const FRotator Rotation = Facing.Rotation();
	AddMesh(Cube, FTransform(Rotation, Center + Facing * 3.0f, FVector(0.06f, (Width + 12.0f) / 100.0f, (Height + 12.0f) / 100.0f)),
		Srgb(40, 38, 36), 0.4f, false);
	const int32 Bands = FMath::Max(Colors.Num(), 1);
	for (int32 I = 0; I < Bands; ++I)
	{
		const float Share = Width / Bands;
		const FVector At = Center + Facing * 6.5f + Along * (-Width / 2 + Share * (I + 0.5f));
		const float Tall = Height * (I % 2 ? 0.62f : 1.0f);
		AddMesh(Cube, FTransform(Rotation, At - FVector(0.0f, 0.0f, (Height - Tall) / 2), FVector(0.02f, Share / 100.0f, Tall / 100.0f)),
			Colors.IsValidIndex(I) ? Colors[I] : FLinearColor::White, 0.8f, false);
	}
}

void ARoomHouse::AddCeiling(const FBox2D& Box, float Height, const TArray<FBox2D>& Holes, const FLinearColor& Color)
{
	const float Top = Height + 30.0f;
	if (Holes.IsEmpty())
	{
		AddBox(FVector(Box.Min, Height), FVector(Box.Max, Top), Color);
		return;
	}
	// Around one hole: strips on either side of it along X, and pieces on
	// either side of it along Y between them.
	const FBox2D& Hole = Holes[0];
	AddBox(FVector(Box.Min.X, Box.Min.Y, Height), FVector(Hole.Min.X, Box.Max.Y, Top), Color);
	AddBox(FVector(Hole.Max.X, Box.Min.Y, Height), FVector(Box.Max.X, Box.Max.Y, Top), Color);
	AddBox(FVector(Hole.Min.X, Box.Min.Y, Height), FVector(Hole.Max.X, Hole.Min.Y, Top), Color);
	AddBox(FVector(Hole.Min.X, Hole.Max.Y, Height), FVector(Hole.Max.X, Box.Max.Y, Top), Color);
}

UPointLightComponent* ARoomHouse::AddPointLight(
	const FVector& Location, const FLinearColor& Color, float Intensity, float Radius, float SourceRadius)
{
	UPointLightComponent* Light = NewObject<UPointLightComponent>(this);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetupAttachment(RootComponent);
	Light->SetRelativeLocation(Location);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(Intensity);
	Light->SetLightColor(Color);
	Light->SetAttenuationRadius(Radius);
	Light->SetSourceRadius(SourceRadius);
	Light->SetSoftSourceRadius(SourceRadius);
	Light->SetCastShadows(true);
	Light->SetVolumetricScatteringIntensity(1.5f);
	Light->RegisterComponent();
	return Light;
}

USpotLightComponent* ARoomHouse::AddSpotLight(
	const FVector& Location, const FRotator& Rotation, const FLinearColor& Color, float Intensity, float Radius, float ConeAngle)
{
	USpotLightComponent* Light = NewObject<USpotLightComponent>(this);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetupAttachment(RootComponent);
	Light->SetRelativeLocationAndRotation(Location, Rotation);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(Intensity);
	Light->SetLightColor(Color);
	Light->SetAttenuationRadius(Radius);
	Light->SetInnerConeAngle(ConeAngle * 0.55f);
	Light->SetOuterConeAngle(ConeAngle);
	Light->SetSourceRadius(4.0f);
	Light->SetCastShadows(true);
	Light->SetVolumetricScatteringIntensity(2.5f);
	Light->RegisterComponent();
	return Light;
}

//
// The sky, and the light through the windows
//

void ARoomHouse::BuildSky()
{
	USkyAtmosphereComponent* Atmosphere = NewObject<USkyAtmosphereComponent>(this);
	Atmosphere->SetupAttachment(RootComponent);
	Atmosphere->RegisterComponent();

	// The afternoon sun, low enough to shine in through the windows, drifting
	// slowly so its patches of light move across the floors.
	SunAim = FRotator(-38.0f, 45.0f, 0.0f);
	Sun = NewObject<UDirectionalLightComponent>(this);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetupAttachment(RootComponent);
	Sun->SetRelativeRotation(SunAim);
	Sun->SetIntensity(9.0f);
	Sun->SetLightColor(Warm(5700.0f));
	Sun->SetAtmosphereSunLight(true);
	Sun->SetLightSourceAngle(1.2f);
	Sun->SetVolumetricScatteringIntensity(2.0f);
	Sun->RegisterComponent();

	USkyLightComponent* Sky = NewObject<USkyLightComponent>(this);
	Sky->SetMobility(EComponentMobility::Movable);
	Sky->SetupAttachment(RootComponent);
	Sky->SourceType = ESkyLightSourceType::SLS_CapturedScene;
	Sky->bRealTimeCapture = true;
	Sky->SetIntensity(1.0f);
	Sky->RegisterComponent();

	if (UMaterialInterface* CloudMaterial = LoadObject<UMaterialInterface>(nullptr, CloudsPath))
	{
		UVolumetricCloudComponent* Clouds = NewObject<UVolumetricCloudComponent>(this);
		Clouds->SetupAttachment(RootComponent);
		Clouds->SetMaterial(CloudMaterial);
		Clouds->RegisterComponent();
	}

	// A faint haze, for the light shafts.
	UExponentialHeightFogComponent* Fog = NewObject<UExponentialHeightFogComponent>(this);
	Fog->SetupAttachment(RootComponent);
	Fog->SetRelativeLocation(FVector(0.0f, 0.0f, -200.0f));
	Fog->SetFogDensity(0.004f);
	Fog->SetFogHeightFalloff(0.08f);
	Fog->SetVolumetricFog(true);
	Fog->SetVolumetricFogScatteringDistribution(0.6f);
	Fog->SetVolumetricFogAlbedo(FColor(255, 248, 236));
	Fog->SetVolumetricFogExtinctionScale(1.2f);
	Fog->SetVolumetricFogDistance(5000.0f);
	Fog->RegisterComponent();

	// The look: soft bloom, a light vignette, and Lumen at its best.
	UPostProcessComponent* Post = NewObject<UPostProcessComponent>(this);
	Post->SetupAttachment(RootComponent);
	Post->bUnbound = true;
	FPostProcessSettings& S = Post->Settings;
	S.bOverride_AutoExposureMinBrightness = true;
	S.AutoExposureMinBrightness = -0.5f;
	S.bOverride_AutoExposureMaxBrightness = true;
	S.AutoExposureMaxBrightness = 2.5f;
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = -0.2f;
	S.bOverride_AutoExposureSpeedUp = true;
	S.AutoExposureSpeedUp = 1.5f;
	S.bOverride_AutoExposureSpeedDown = true;
	S.AutoExposureSpeedDown = 1.0f;
	// Warm light reads as white, as the eye takes it, rather than yellow.
	S.bOverride_WhiteTemp = true;
	S.WhiteTemp = 5600.0f;
	S.bOverride_BloomIntensity = true;
	S.BloomIntensity = 0.45f;
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = 0.3f;
	S.bOverride_FilmGrainIntensity = true;
	S.FilmGrainIntensity = 0.06f;
	S.bOverride_MotionBlurAmount = true;
	S.MotionBlurAmount = 0.25f;
	S.bOverride_ColorSaturation = true;
	S.ColorSaturation = FVector4(1.04f, 1.04f, 1.04f, 1.0f);
	S.bOverride_ColorContrast = true;
	S.ColorContrast = FVector4(1.06f, 1.06f, 1.06f, 1.0f);
	S.bOverride_LumenSceneLightingQuality = true;
	S.LumenSceneLightingQuality = 2.0f;
	S.bOverride_LumenSceneDetail = true;
	S.LumenSceneDetail = 2.0f;
	S.bOverride_LumenFinalGatherQuality = true;
	S.LumenFinalGatherQuality = 2.0f;
	S.bOverride_LumenReflectionQuality = true;
	S.LumenReflectionQuality = 2.0f;
	S.bOverride_LumenMaxTraceDistance = true;
	S.LumenMaxTraceDistance = 6000.0f;
	Post->RegisterComponent();
}

void ARoomHouse::BuildOutside()
{
	// The ground around the house, and soft hills far off, through the
	// windows. And a few trees close by.
	AddBox(FVector(-40000.0f, -40000.0f, -60.0f), FVector(40000.0f, 40000.0f, -12.0f), Srgb(206, 192, 162), 0.9f);
	const FLinearColor Hills[] = {Srgb(150, 172, 120), Srgb(132, 158, 112), Srgb(168, 180, 132)};
	const FVector HillAt[] = {
		FVector(14000.0f, -9000.0f, -1200.0f), FVector(18000.0f, 4000.0f, -1500.0f), FVector(-6000.0f, -16000.0f, -1400.0f),
		FVector(4000.0f, 17000.0f, -1300.0f), FVector(-15000.0f, 6000.0f, -1500.0f), FVector(-12000.0f, -12000.0f, -1600.0f),
	};
	for (int32 I = 0; I < UE_ARRAY_COUNT(HillAt); ++I)
	{
		AddSphere(HillAt[I], FVector(6000.0f + 900.0f * I, 5200.0f, 2600.0f), Hills[I % 3], 0.9f, false);
	}
	const FVector TreeAt[] = {
		FVector(-200.0f, -2900.0f, -12.0f), FVector(450.0f, -3300.0f, -12.0f), FVector(-750.0f, -3500.0f, -12.0f),
		FVector(2900.0f, -500.0f, -12.0f), FVector(3200.0f, 700.0f, -12.0f), FVector(2600.0f, 1700.0f, -12.0f),
		FVector(-200.0f, 2900.0f, -12.0f), FVector(500.0f, 3400.0f, -12.0f), FVector(-3000.0f, -300.0f, -12.0f),
		FVector(-3300.0f, 900.0f, -12.0f), FVector(-2800.0f, -1300.0f, -12.0f),
	};
	for (int32 I = 0; I < UE_ARRAY_COUNT(TreeAt); ++I)
	{
		const float Tall = 700.0f + 180.0f * (I % 3);
		AddCylinder(TreeAt[I], 22.0f, 160.0f, Bark, 0.9f, false);
		AddMesh(Cone, FTransform(FRotator::ZeroRotator, TreeAt[I] + FVector(0.0f, 0.0f, 120.0f + Tall / 2), FVector(3.4f, 3.4f, Tall / 100.0f)),
			I % 2 ? LeafDark : Leaf, 0.85f, false);
	}
}

//
// The rooms
//

void ARoomHouse::BuildHall()
{
	const float Height = 900.0f;
	const float Top = Height + Wall;
	// A polished stone floor, and a round rug in the middle.
	AddBox(FVector(-800.0f, -800.0f, -10.0f), FVector(800.0f, 800.0f, 0.0f), Stone, 0.28f);
	AddCylinder(FVector(0.0f, 0.0f, 0.0f), 330.0f, 1.2f, Srgb(184, 134, 120), 0.9f, false);

	// Stone-gray below a rail, white above, dark skirting, white frames.
	Style = {Srgb(168, 164, 158), 120.0f, Skirting, Srgb(244, 242, 238)};
	AddWall(FVector2D(800.0f, -920.0f), FVector2D(800.0f, 920.0f), Top, {{920.0f, 360.0f, 0.0f, 420.0f}}, Plaster);
	AddWall(FVector2D(-800.0f, -820.0f), FVector2D(-800.0f, 820.0f), Top, {{820.0f, 300.0f, 0.0f, 400.0f}}, Plaster);
	AddWall(FVector2D(-820.0f, -800.0f), FVector2D(820.0f, -800.0f), Top, {{820.0f, 220.0f, 0.0f, 300.0f}}, Plaster);
	AddWall(FVector2D(-820.0f, 800.0f), FVector2D(820.0f, 800.0f), Top, {{820.0f, 220.0f, 0.0f, 300.0f}}, Plaster);

	// Pictures either side of the doorways to the kitchen and the music room.
	AddPicture(FVector(-450.0f, -778.0f, 250.0f), FVector(0.0f, 1.0f, 0.0f), 200.0f, 140.0f,
		{Srgb(214, 120, 92), Srgb(236, 196, 120), Srgb(92, 120, 150)});
	AddPicture(FVector(450.0f, -778.0f, 250.0f), FVector(0.0f, 1.0f, 0.0f), 160.0f, 200.0f, {Srgb(70, 96, 120), Srgb(226, 214, 196)});
	AddPicture(FVector(-450.0f, 778.0f, 250.0f), FVector(0.0f, -1.0f, 0.0f), 160.0f, 200.0f, {Srgb(232, 176, 80), Srgb(120, 150, 110)});
	AddPicture(FVector(450.0f, 778.0f, 250.0f), FVector(0.0f, -1.0f, 0.0f), 200.0f, 140.0f,
		{Srgb(160, 92, 120), Srgb(230, 220, 200), Srgb(200, 110, 80)});

	// A skylight in the middle, with a deep well around it.
	const FBox2D Skylight(FVector2D(-260.0f, -260.0f), FVector2D(260.0f, 260.0f));
	AddCeiling(FBox2D(FVector2D(-820.0f, -820.0f), FVector2D(820.0f, 820.0f)), Height, {Skylight}, Ceiling);
	AddBox(FVector(-280.0f, -280.0f, Height + 30.0f), FVector(280.0f, -260.0f, Height + 160.0f), Ceiling);
	AddBox(FVector(-280.0f, 260.0f, Height + 30.0f), FVector(280.0f, 280.0f, Height + 160.0f), Ceiling);
	AddBox(FVector(-280.0f, -260.0f, Height + 30.0f), FVector(-260.0f, 260.0f, Height + 160.0f), Ceiling);
	AddBox(FVector(260.0f, -260.0f, Height + 30.0f), FVector(280.0f, 260.0f, Height + 160.0f), Ceiling);

	// Columns, clear of the paths between the doorways.
	for (float X : {-480.0f, 480.0f})
	{
		for (float Y : {-480.0f, 480.0f})
		{
			AddCylinder(FVector(X, Y, 0.0f), 45.0f, Height, Column, 0.5f);
			AddCylinder(FVector(X, Y, 0.0f), 58.0f, 30.0f, Column, 0.5f);
		}
	}

	// Benches along the walls, and big potted shrubs in the corners.
	for (float Y : {-720.0f, 720.0f})
	{
		AddBox(FVector(-600.0f, Y - 30.0f, 0.0f), FVector(-340.0f, Y + 30.0f, 44.0f), Srgb(150, 112, 86), 0.55f);
	}
	for (float Y : {-660.0f, 660.0f})
	{
		AddCylinder(FVector(660.0f, Y, 0.0f), 48.0f, 62.0f, Srgb(176, 110, 82), 0.7f);
		AddSphere(FVector(660.0f, Y, 140.0f), FVector(80.0f, 80.0f, 95.0f), Leaf, 0.85f, false);
	}

	// Two lamps hanging from the ceiling on long cords, swinging gently.
	for (int32 I = 0; I < 2; ++I)
	{
		const float Y = I == 0 ? -340.0f : 340.0f;
		USceneComponent* Pivot = NewObject<USceneComponent>(this);
		Pivot->SetMobility(EComponentMobility::Movable);
		Pivot->SetupAttachment(RootComponent);
		Pivot->SetRelativeLocation(FVector(0.0f, Y, Height));
		Pivot->RegisterComponent();
		const float Drop = 360.0f;
		UStaticMeshComponent* Cord = NewObject<UStaticMeshComponent>(this);
		Cord->SetMobility(EComponentMobility::Movable);
		Cord->SetStaticMesh(Cylinder);
		Cord->SetupAttachment(Pivot);
		Cord->SetRelativeLocation(FVector(0.0f, 0.0f, -Drop / 2));
		Cord->SetRelativeScale3D(FVector(0.03f, 0.03f, Drop / 100.0f));
		Cord->SetMaterial(0, GetMaterial(Charcoal, 0.5f, 0.0f, FLinearColor::Black));
		Cord->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Cord->RegisterComponent();
		UStaticMeshComponent* Shade = NewObject<UStaticMeshComponent>(this);
		Shade->SetMobility(EComponentMobility::Movable);
		Shade->SetStaticMesh(Sphere);
		Shade->SetupAttachment(Pivot);
		Shade->SetRelativeLocation(FVector(0.0f, 0.0f, -Drop - 30.0f));
		Shade->SetRelativeScale3D(FVector(0.7f));
		// Its own material, to change color at a party.
		UMaterialInstanceDynamic* ShadeMaterial = UMaterialInstanceDynamic::Create(Flat ? Flat.Get() : UMaterial::GetDefaultMaterial(MD_Surface), this);
		ShadeMaterial->SetVectorParameterValue(TEXT("Color"), Srgb(255, 236, 206));
		ShadeMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.4f);
		ShadeMaterial->SetVectorParameterValue(TEXT("Emissive"), Srgb(255, 214, 160));
		ShadeMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"), 30.0f);
		Shade->SetMaterial(0, ShadeMaterial);
		HallShades.Add(ShadeMaterial);
		Shade->SetCastShadow(false);
		Shade->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Shade->RegisterComponent();
		UPointLightComponent* Light = AddPointLight(FVector::ZeroVector, Warm(2900.0f), 60.0f, 1800.0f, 30.0f);
		Light->AttachToComponent(Shade, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		HallLamps.Add(Light);
		Swinging.Add({Pivot, 7.0f, 5.6f + 0.7f * I, 0.37f * I});
	}
}

void ARoomHouse::BuildConservatory()
{
	const float Height = 700.0f;
	const float Top = Height + Wall;
	AddBox(FVector(800.0f, -920.0f, -10.0f), FVector(2220.0f, 920.0f, 0.0f), Srgb(184, 124, 96), 0.5f);

	// Tall windows all round, and a glass roof: only its beams.
	const FLinearColor Sage = Srgb(188, 202, 180);
	Style = {Srgb(132, 156, 126), 90.0f, Srgb(88, 108, 84), Srgb(242, 242, 236)};
	AddWall(FVector2D(2200.0f, -920.0f), FVector2D(2200.0f, 920.0f), Top,
		{{320.0f, 300.0f, 40.0f, 600.0f}, {720.0f, 300.0f, 40.0f, 600.0f}, {1120.0f, 300.0f, 40.0f, 600.0f}, {1520.0f, 300.0f, 40.0f, 600.0f}},
		Sage);
	for (float Y : {-900.0f, 900.0f})
	{
		AddWall(FVector2D(800.0f, Y), FVector2D(2220.0f, Y), Top,
			{{350.0f, 280.0f, 40.0f, 600.0f}, {750.0f, 280.0f, 40.0f, 600.0f}, {1150.0f, 280.0f, 40.0f, 600.0f}}, Sage);
	}
	const FLinearColor Frame = Srgb(236, 234, 226);
	AddBox(FVector(820.0f, -920.0f, Height), FVector(2220.0f, -860.0f, Height + 30.0f), Frame);
	AddBox(FVector(820.0f, 860.0f, Height), FVector(2220.0f, 920.0f, Height + 30.0f), Frame);
	AddBox(FVector(2140.0f, -860.0f, Height), FVector(2220.0f, 860.0f, Height + 30.0f), Frame);
	for (float X = 1000.0f; X < 2150.0f; X += 190.0f)
	{
		AddBox(FVector(X - 12.0f, -860.0f, Height), FVector(X + 12.0f, 860.0f, Height + 30.0f), Frame);
	}
	for (float Y : {-430.0f, 0.0f, 430.0f})
	{
		AddBox(FVector(820.0f, Y - 8.0f, Height + 4.0f), FVector(2140.0f, Y + 8.0f, Height + 26.0f), Frame);
	}

	// Planters with trees, and a fountain.
	const FLinearColor Planter = Srgb(150, 106, 84);
	int32 Tree = 0;
	for (float X : {1250.0f, 1950.0f})
	{
		for (float Y : {-600.0f, 600.0f})
		{
			AddBox(FVector(X - 130.0f, Y - 120.0f, 0.0f), FVector(X + 130.0f, Y + 120.0f, 70.0f), Planter, 0.7f);
			AddBox(FVector(X - 118.0f, Y - 108.0f, 70.0f), FVector(X + 118.0f, Y + 108.0f, 74.0f), Srgb(92, 70, 56), 0.95f, false);
			const float Tall = 260.0f + 40.0f * (Tree % 2);
			AddCylinder(FVector(X, Y, 70.0f), 11.0f, Tall, Bark, 0.85f);
			// Its canopy, which grows when it's watered.
			for (UStaticMeshComponent* Canopy : {
					 AddSphere(FVector(X, Y, 70.0f + Tall + 40.0f), FVector(115.0f, 115.0f, 100.0f), Tree % 2 ? LeafLight : Leaf, 0.85f, false),
					 AddSphere(FVector(X + 40.0f, Y - 30.0f, 70.0f + Tall + 120.0f), FVector(78.0f), LeafDark, 0.85f, false)})
			{
				Canopy->SetMobility(EComponentMobility::Movable);
				Canopies.Add(Canopy);
				CanopySizes.Add(Canopy->GetRelativeScale3D());
			}
			Growth.Add(1.0f);
			Grown.Add(1.0f);
			++Tree;
		}
	}
	const FVector Fountain(1700.0f, 0.0f, 0.0f);
	AddCylinder(Fountain, 140.0f, 50.0f, Srgb(220, 214, 204), 0.5f);
	AddCylinder(Fountain + FVector(0.0f, 0.0f, 1.0f), 126.0f, 50.0f, Srgb(64, 112, 124), 0.06f, false);
	AddCylinder(Fountain, 18.0f, 120.0f, Srgb(220, 214, 204), 0.5f);
	AddSphere(Fountain + FVector(0.0f, 0.0f, 140.0f), FVector(30.0f), Srgb(220, 214, 204), 0.3f);

	// A bench by the fountain.
	AddBox(FVector(1580.0f, 520.0f, 0.0f), FVector(1820.0f, 580.0f, 44.0f), Srgb(150, 112, 86), 0.55f);
}

void ARoomHouse::BuildKitchen()
{
	const float Height = 450.0f;
	const float Top = Height + Wall;
	AddBox(FVector(-620.0f, -2020.0f, -10.0f), FVector(620.0f, -800.0f, 0.0f), Srgb(176, 150, 124), 0.45f);
	const FLinearColor Cream = Srgb(234, 228, 214);
	Style = {Srgb(196, 136, 110), 110.0f, Srgb(140, 88, 68), Srgb(244, 240, 232)};
	AddWall(FVector2D(-620.0f, -2000.0f), FVector2D(620.0f, -2000.0f), Top, {{620.0f, 420.0f, 100.0f, 330.0f}}, Cream);
	AddWall(FVector2D(600.0f, -2020.0f), FVector2D(600.0f, -800.0f), Top, {{620.0f, 300.0f, 100.0f, 330.0f}}, Cream);
	AddWall(FVector2D(-600.0f, -2020.0f), FVector2D(-600.0f, -800.0f), Top, {}, Cream);
	AddCeiling(FBox2D(FVector2D(-620.0f, -2020.0f), FVector2D(620.0f, -780.0f)), Height, {}, Ceiling);

	// Counters under the window, and cupboards either side of it.
	const FLinearColor Terracotta = Srgb(186, 112, 88);
	const FLinearColor Worktop = Srgb(244, 242, 236);
	AddBox(FVector(-560.0f, -1980.0f, 0.0f), FVector(560.0f, -1915.0f, 90.0f), Terracotta, 0.5f);
	AddBox(FVector(-565.0f, -1985.0f, 90.0f), FVector(565.0f, -1905.0f, 95.0f), Worktop, 0.2f);
	AddBox(FVector(-560.0f, -1980.0f, 170.0f), FVector(-230.0f, -1940.0f, 260.0f), Terracotta, 0.5f);
	AddBox(FVector(230.0f, -1980.0f, 170.0f), FVector(560.0f, -1940.0f, 260.0f), Terracotta, 0.5f);

	// The island, with three lamps over it.
	AddBox(FVector(-170.0f, -1560.0f, 0.0f), FVector(170.0f, -1440.0f, 90.0f), Terracotta, 0.5f);
	AddBox(FVector(-185.0f, -1575.0f, 90.0f), FVector(185.0f, -1425.0f, 95.0f), Worktop, 0.18f);
	for (float X : {-110.0f, 0.0f, 110.0f})
	{
		AddBox(FVector(X - 0.8f, -1500.8f, 232.0f), FVector(X + 0.8f, -1499.2f, Height), Charcoal, 0.5f, false);
		AddLamp(FVector(X, -1500.0f, 220.0f), 14.0f, Srgb(255, 226, 180), 40.0f);
		AddPointLight(FVector(X, -1500.0f, 216.0f), Warm(2700.0f), 14.0f, 900.0f, 10.0f);
	}

	// A small table, by the door.
	const FLinearColor Wood = Srgb(150, 112, 86);
	AddBox(FVector(-500.0f, -1160.0f, 72.0f), FVector(-260.0f, -950.0f, 77.0f), Wood, 0.5f);
	for (float X : {-485.0f, -275.0f})
	{
		for (float Y : {-1145.0f, -965.0f})
		{
			AddBox(FVector(X - 4.0f, Y - 4.0f, 0.0f), FVector(X + 4.0f, Y + 4.0f, 72.0f), Wood, 0.5f);
		}
	}
	for (float Y : {-1200.0f, -910.0f})
	{
		AddCylinder(FVector(-380.0f, Y, 0.0f), 20.0f, 46.0f, Srgb(232, 196, 116), 0.6f);
	}
}

void ARoomHouse::BuildMusicRoom()
{
	const float Height = 500.0f;
	const float Top = Height + Wall;
	AddBox(FVector(-620.0f, 800.0f, -10.0f), FVector(620.0f, 2020.0f, 0.0f), Srgb(96, 70, 54), 0.3f);
	const FLinearColor Indigo = Srgb(118, 128, 176);
	Style = {Srgb(54, 58, 100), 130.0f, Srgb(30, 30, 44), Srgb(226, 222, 214)};
	AddWall(FVector2D(-620.0f, 2000.0f), FVector2D(620.0f, 2000.0f), Top,
		{{320.0f, 220.0f, 90.0f, 400.0f}, {920.0f, 220.0f, 90.0f, 400.0f}}, Indigo);
	AddWall(FVector2D(600.0f, 800.0f), FVector2D(600.0f, 2020.0f), Top, {{500.0f, 300.0f, 90.0f, 400.0f}}, Indigo);
	AddWall(FVector2D(-600.0f, 800.0f), FVector2D(-600.0f, 2020.0f), Top, {}, Indigo);
	AddCeiling(FBox2D(FVector2D(-620.0f, 780.0f), FVector2D(620.0f, 2020.0f)), Height, {}, Srgb(196, 198, 214));

	// A low stage, speakers, and a piano with its bench.
	const FLinearColor Black = Srgb(30, 30, 34);
	AddBox(FVector(-520.0f, 1650.0f, 0.0f), FVector(520.0f, 1980.0f, 25.0f), Srgb(44, 44, 50), 0.4f);
	for (float X : {-470.0f, 470.0f})
	{
		AddBox(FVector(X - 28.0f, 1900.0f, 25.0f), FVector(X + 28.0f, 1960.0f, 150.0f), Black, 0.6f);
		AddCylinder(FVector(X, 1898.0f, 100.0f), 16.0f, 2.0f, Srgb(90, 90, 96), 0.4f, false);
	}
	// An upright piano, its keys at standing height, for whoever plays it.
	AddBox(FVector(-450.0f, 1150.0f, 0.0f), FVector(-290.0f, 1300.0f, 132.0f), Srgb(18, 18, 20), 0.12f);
	AddBox(FVector(-290.0f, 1155.0f, 80.0f), FVector(-254.0f, 1295.0f, 92.0f), Srgb(18, 18, 20), 0.12f);
	AddBox(FVector(-290.0f, 1160.0f, 92.0f), FVector(-257.0f, 1290.0f, 96.0f), Srgb(246, 244, 238), 0.3f);

	// Stage lights sweeping across the stage, in color, and a warm lamp in
	// the corner.
	const FLinearColor Colors[] = {Srgb(255, 70, 160), Srgb(70, 190, 255), Srgb(255, 170, 60)};
	for (int32 I = 0; I < 3; ++I)
	{
		const float X = -260.0f + 260.0f * I;
		const FVector At(X, 1230.0f, Height - 20.0f);
		AddBox(At - FVector(14.0f, 14.0f, 0.0f), At + FVector(14.0f, 14.0f, 20.0f), Black, 0.5f, false);
		const FRotator Aim(-48.0f, 90.0f - 12.0f * (I - 1), 0.0f);
		USpotLightComponent* Light = AddSpotLight(At, Aim, Colors[I], 900.0f, 2000.0f, 16.0f);
		Sweeping.Add({Light, Aim, 22.0f, 7.0f + 1.3f * I, 0.29f * I});
	}
	AddCylinder(FVector(470.0f, 900.0f, 0.0f), 2.0f, 160.0f, Black, 0.5f, false);
	UStaticMeshComponent* Shade = AddCylinder(FVector(470.0f, 900.0f, 160.0f), 22.0f, 32.0f, Srgb(255, 230, 190), 0.6f, false);
	Shade->SetMaterial(0, GetMaterial(Srgb(255, 230, 190), 0.6f, 20.0f, Srgb(255, 200, 140)));
	Shade->SetCastShadow(false);
	AddPointLight(FVector(470.0f, 900.0f, 176.0f), Warm(2600.0f), 14.0f, 1000.0f, 12.0f);
	// Soft light from the ceiling, warm sconces on the side wall, and a lamp
	// over the piano.
	for (float Y : {1100.0f, 1600.0f})
	{
		AddSpotLight(FVector(150.0f, Y, Height - 5.0f), FRotator(-90.0f, 0.0f, 0.0f), Warm(3000.0f), 260.0f, 1200.0f, 60.0f);
	}
	for (float Y : {1050.0f, 1550.0f})
	{
		AddLamp(FVector(-570.0f, Y, 230.0f), 12.0f, Srgb(255, 222, 180), 30.0f);
		AddPointLight(FVector(-555.0f, Y, 230.0f), Warm(2700.0f), 40.0f, 1000.0f, 10.0f);
	}
	AddBox(FVector(-371.0f, 1224.0f, 250.0f), FVector(-369.0f, 1226.0f, Height), Charcoal, 0.5f, false);
	AddLamp(FVector(-370.0f, 1225.0f, 240.0f), 16.0f, Srgb(255, 226, 186), 35.0f);
	AddPointLight(FVector(-370.0f, 1225.0f, 236.0f), Warm(2800.0f), 16.0f, 900.0f, 12.0f);
	AddPicture(FVector(-578.0f, 1300.0f, 270.0f), FVector(1.0f, 0.0f, 0.0f), 220.0f, 150.0f,
		{Srgb(232, 196, 96), Srgb(214, 96, 120), Srgb(96, 170, 200)});
}

void ARoomHouse::BuildGallery()
{
	const float Height = 600.0f;
	const float Top = Height + Wall;
	AddBox(FVector(-2220.0f, -720.0f, -10.0f), FVector(-800.0f, 720.0f, 0.0f), Srgb(214, 212, 208), 0.22f);
	const FLinearColor White = Srgb(236, 235, 232);
	Style = {White, 0.0f, Skirting, Srgb(236, 235, 232)};
	AddWall(FVector2D(-2200.0f, -720.0f), FVector2D(-2200.0f, 720.0f), Top, {{720.0f, 360.0f, 60.0f, 520.0f}}, White);
	for (float Y : {-700.0f, 700.0f})
	{
		AddWall(FVector2D(-2220.0f, Y), FVector2D(-780.0f, Y), Top,
			{{320.0f, 220.0f, 380.0f, 560.0f}, {720.0f, 220.0f, 380.0f, 560.0f}, {1120.0f, 220.0f, 380.0f, 560.0f}}, White);
	}
	AddCeiling(FBox2D(FVector2D(-2220.0f, -720.0f), FVector2D(-780.0f, 720.0f)), Height, {}, Ceiling);

	// Plinths with sculptures, each under its own light, and a bench.
	const FLinearColor Colors[] = {
		Srgb(232, 120, 100), Srgb(226, 182, 72), Srgb(60, 150, 150), Srgb(176, 150, 210), Srgb(236, 236, 230), Srgb(90, 120, 200),
	};
	int32 Piece = 0;
	for (float X : {-1900.0f, -1500.0f, -1100.0f})
	{
		for (float Y : {-470.0f, 470.0f})
		{
			AddBox(FVector(X - 45.0f, Y - 45.0f, 0.0f), FVector(X + 45.0f, Y + 45.0f, 100.0f), Charcoal, 0.4f);
			const FLinearColor& Color = Colors[Piece % 6];
			UStaticMeshComponent* Sculpture = nullptr;
			switch (Piece % 3)
			{
			case 0:
				Sculpture = AddMesh(Sphere, FTransform(FRotator::ZeroRotator, FVector(X, Y, 136.0f), FVector(0.72f, 0.72f, 0.6f)), Color, 0.25f);
				break;
			case 1:
				Sculpture = AddMesh(Cone, FTransform(FRotator(0.0f, 0.0f, 12.0f), FVector(X, Y, 145.0f), FVector(0.7f, 0.5f, 0.9f)), Color, 0.35f);
				break;
			default:
				Sculpture = AddMesh(Cube, FTransform(FRotator(35.0f, 45.0f, 35.0f), FVector(X, Y, 150.0f), FVector(0.48f)), Color, 0.3f);
				break;
			}
			// Each spins, when the player gives it a push.
			Sculpture->SetMobility(EComponentMobility::Movable);
			Sculptures.Add(Sculpture);
			AddSpotLight(FVector(X, Y * 0.8f, Height - 10.0f), (FVector(X, Y, 130.0f) - FVector(X, Y * 0.8f, Height - 10.0f)).Rotation(),
				Warm(4300.0f), 160.0f, 900.0f, 18.0f);
			++Piece;
		}
	}
	AddBox(FVector(-1650.0f, -45.0f, 0.0f), FVector(-1350.0f, 45.0f, 44.0f), Srgb(150, 112, 86), 0.55f);

	// Big canvases on the long walls, under the high windows.
	const TArray<FLinearColor> Canvases[] = {
		{Srgb(40, 70, 110), Srgb(220, 120, 90), Srgb(236, 226, 206), Srgb(70, 130, 120)},
		{Srgb(236, 196, 100), Srgb(60, 60, 70), Srgb(200, 80, 90)},
		{Srgb(120, 160, 200), Srgb(240, 232, 216), Srgb(232, 150, 110)},
		{Srgb(90, 130, 90), Srgb(220, 210, 180), Srgb(170, 90, 140), Srgb(60, 80, 120)},
	};
	AddPicture(FVector(-1700.0f, -678.0f, 230.0f), FVector(0.0f, 1.0f, 0.0f), 260.0f, 170.0f, Canvases[0]);
	AddPicture(FVector(-1300.0f, -678.0f, 230.0f), FVector(0.0f, 1.0f, 0.0f), 180.0f, 220.0f, Canvases[1]);
	AddPicture(FVector(-1700.0f, 678.0f, 230.0f), FVector(0.0f, -1.0f, 0.0f), 180.0f, 220.0f, Canvases[2]);
	AddPicture(FVector(-1300.0f, 678.0f, 230.0f), FVector(0.0f, -1.0f, 0.0f), 260.0f, 170.0f, Canvases[3]);
}

//
// Each frame
//

void ARoomHouse::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;

	if (Sun && SunDriftSeconds > 0.0f)
	{
		const float Drift = FMath::Sin(UE_TWO_PI * Time / SunDriftSeconds);
		Sun->SetRelativeRotation(FRotator(SunAim.Pitch + 4.0f * Drift, SunAim.Yaw + 22.0f * Drift, 0.0f));
	}
	for (const FSwinging& Lamp : Swinging)
	{
		const float Angle = Lamp.Amplitude * FMath::Sin(UE_TWO_PI * (Time / Lamp.Period + Lamp.Phase));
		const float Across = 0.35f * Lamp.Amplitude * FMath::Sin(UE_TWO_PI * (Time / (Lamp.Period * 1.37f) + Lamp.Phase));
		Lamp.Pivot->SetRelativeRotation(FRotator(0.0f, 0.0f, Angle) + FRotator(Across, 0.0f, 0.0f));
	}
	// The trees grow, slowly, once they've been watered.
	for (int32 Planter = 0; Planter < Growth.Num(); ++Planter)
	{
		Grown[Planter] = FMath::FInterpTo(Grown[Planter], Growth[Planter], DeltaSeconds, 0.6f);
		for (int32 I = 0; I < 2 && Canopies.IsValidIndex(2 * Planter + I); ++I)
		{
			Canopies[2 * Planter + I]->SetRelativeScale3D(CanopySizes[2 * Planter + I] * Grown[Planter]);
		}
	}

	// At a party, the hall's lamps change color with the music, and pulse with its beat.
	Party = FMath::FInterpTo(Party, PartyTarget, DeltaSeconds, 2.0f);
	for (int32 I = 0; I < HallLamps.Num(); ++I)
	{
		const float Hue = FMath::Frac(static_cast<float>(PartyBeat) / 8.0f + 0.5f * I) * 360.0f;
		const FLinearColor Party_ = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue / 360.0f * 255.0f), 200, 255);
		const FLinearColor Color = FMath::Lerp(Warm(2900.0f), Party_, Party);
		const float Pulse = FMath::Exp(-4.0f * static_cast<float>(FMath::Frac(PartyBeat)));
		HallLamps[I]->SetLightColor(Color);
		HallLamps[I]->SetIntensity(60.0f * (1.0f + Party * (0.8f * Pulse + 0.6f * PartyLoudness)));
		if (HallShades.IsValidIndex(I))
		{
			HallShades[I]->SetVectorParameterValue(TEXT("Emissive"), FMath::Lerp(Srgb(255, 214, 160), Party_, Party));
			HallShades[I]->SetScalarParameterValue(TEXT("EmissiveStrength"), 30.0f * (1.0f + Party * Pulse));
		}
	}

	for (const FSweeping& Light : Sweeping)
	{
		const float Yaw = Light.Amplitude * FMath::Sin(UE_TWO_PI * (Time / Light.Period + Light.Phase));
		const float Pitch = 6.0f * FMath::Sin(UE_TWO_PI * (Time / (Light.Period * 0.61f) + Light.Phase));
		Light.Light->SetRelativeRotation(Light.Aim + FRotator(Pitch, Yaw, 0.0f));
	}
}

//
// Finding the way
//

const FRoomArea* ARoomHouse::FindArea(FName Id) const
{
	return Areas.FindByPredicate([Id](const FRoomArea& Area) { return Area.Id == Id; });
}

FName ARoomHouse::AreaAt(const FVector& Location) const
{
	const FVector2D Point(Location);
	for (const FRoomArea& Area : Areas)
	{
		if (Area.Box.ExpandBy(Wall / 2).IsInside(Point))
		{
			return Area.Id;
		}
	}
	return NAME_None;
}

FRoomSpot ARoomHouse::GetPlayerStart() const
{
	return {FVector(-430.0f, 0.0f, 0.0f), 0.0f};
}

FRoomSpot ARoomHouse::GetHome(FName Area) const
{
	if (const FRoomSpot* Home = Homes.Find(Area))
	{
		return *Home;
	}
	const FRoomArea* Found = FindArea(Area);
	return {Found ? FVector(Found->Hub, 0.0f) : FVector::ZeroVector, 0.0f};
}

FVector ARoomHouse::KeepInside(const FVector& Location) const
{
	const FVector2D Point(Location);
	const FRoomArea* Best = nullptr;
	float BestDistance = TNumericLimits<float>::Max();
	for (const FRoomArea& Area : Areas)
	{
		const FBox2D Inner = Area.Box.ExpandBy(-Margin);
		const float Distance = FVector2D::DistSquared(Point, Inner.GetClosestPointTo(Point));
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = &Area;
		}
	}
	if (!Best)
	{
		return Location;
	}
	const FVector2D Inside = Best->Box.ExpandBy(-Margin).GetClosestPointTo(Point);
	return FVector(Inside, Location.Z);
}

void ARoomHouse::FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPath, TConstArrayView<FBox2D> Avoid) const
{
	// Through the doorways, and in each room on the way, around the furniture.
	TArray<FVector> Doorways;
	FindDoorways(From, To, Doorways);
	OutPath.Reset();
	FVector2D At(From);
	for (const FVector& Next : Doorways)
	{
		const FName Area = AreaAt(FVector(At, 0.0f));
		if (!Area.IsNone() && Area == AreaAt(Next))
		{
			TArray<FVector2D> Around;
			FindAround(At, FVector2D(Next), Area, Avoid, Around);
			for (int32 I = 0; I < Around.Num() - 1; ++I)
			{
				OutPath.Add(FVector(Around[I], Next.Z));
			}
		}
		OutPath.Add(Next);
		At = FVector2D(Next);
	}
}

float ARoomHouse::PathLength(const FVector& From, const FVector& To) const
{
	TArray<FVector> Path;
	FindPath(From, To, Path);
	float Length = 0.0f;
	FVector At = From;
	for (const FVector& Next : Path)
	{
		Length += FVector::Dist2D(At, Next);
		At = Next;
	}
	return Length;
}

void ARoomHouse::FindAround(
	const FVector2D& From, const FVector2D& To, FName AreaId, TConstArrayView<FBox2D> Avoid, TArray<FVector2D>& OutPath) const
{
	OutPath.Reset();
	const FRoomArea* Area = FindArea(AreaId);
	if (!Area)
	{
		OutPath.Add(To);
		return;
	}

	// The furniture in this room, as wide as a character. Whatever they start
	// or end up right by (e.g. the stove they cook at) is only as wide as it
	// is, so they can get to it, or away from it.
	TArray<FBox2D> InWay;
	TArray<FBox2D> Turns;
	TArray<FBox2D> All = Blockers;
	All.Append(Avoid.GetData(), Avoid.Num());
	for (const FBox2D& Blocker : All)
	{
		FBox2D Wide = Blocker.ExpandBy(Clearance);
		if (!Wide.Intersect(Area->Box))
		{
			continue;
		}
		if (Wide.IsInside(From) || Wide.IsInside(To))
		{
			Wide = Blocker.ExpandBy(Clearance * 0.25f);
			if (Wide.IsInside(From) || Wide.IsInside(To))
			{
				continue;
			}
		}
		InWay.Add(Wide);
		Turns.Add(Blocker.ExpandBy(CornerClearance));
	}
	auto Clear = [&InWay](const FVector2D& A, const FVector2D& B) {
		for (const FBox2D& Box : InWay)
		{
			if (Crosses(A, B, Box))
			{
				return false;
			}
		}
		return true;
	};
	if (Clear(From, To))
	{
		OutPath.Add(To);
		return;
	}

	// Otherwise, the shortest way by the furniture's corners: those in the
	// room, away from its walls, and not in other furniture.
	TArray<FVector2D> Nodes = {From, To};
	const FBox2D Room = Area->Box.ExpandBy(-Wall * 0.5f - Clearance * 0.75f);
	for (const FBox2D& Turn : Turns)
	{
		for (const FVector2D& Corner : {Turn.Min, Turn.Max, FVector2D(Turn.Min.X, Turn.Max.Y), FVector2D(Turn.Max.X, Turn.Min.Y)})
		{
			if (!Room.IsInside(Corner))
			{
				continue;
			}
			bool bFree = true;
			for (const FBox2D& Box : InWay)
			{
				bFree &= !Box.IsInside(Corner);
			}
			if (bFree)
			{
				Nodes.Add(Corner);
			}
		}
	}
	TArray<float> Cost;
	TArray<int32> Previous;
	TArray<bool> Done;
	Cost.Init(TNumericLimits<float>::Max(), Nodes.Num());
	Previous.Init(INDEX_NONE, Nodes.Num());
	Done.Init(false, Nodes.Num());
	Cost[0] = 0.0f;
	for (;;)
	{
		int32 Best = INDEX_NONE;
		for (int32 I = 0; I < Nodes.Num(); ++I)
		{
			if (!Done[I] && Cost[I] < TNumericLimits<float>::Max() && (Best == INDEX_NONE || Cost[I] < Cost[Best]))
			{
				Best = I;
			}
		}
		if (Best == INDEX_NONE || Best == 1)
		{
			break;
		}
		Done[Best] = true;
		for (int32 I = 0; I < Nodes.Num(); ++I)
		{
			if (Done[I])
			{
				continue;
			}
			const float Through = Cost[Best] + FVector2D::Distance(Nodes[Best], Nodes[I]);
			if (Through < Cost[I] && Clear(Nodes[Best], Nodes[I]))
			{
				Cost[I] = Through;
				Previous[I] = Best;
			}
		}
	}
	if (Previous[1] == INDEX_NONE)
	{
		// No way round: straight there, and they'll find their way along.
		OutPath.Add(To);
		return;
	}
	for (int32 I = 1; I != 0; I = Previous[I])
	{
		OutPath.Insert(Nodes[I], 0);
	}
}

FVector ARoomHouse::ClearOf(const FVector& Location) const
{
	FVector2D Point(Location);
	for (const FBox2D& Blocker : Blockers)
	{
		const FBox2D Wide = Blocker.ExpandBy(Clearance);
		if (!Wide.IsInside(Point))
		{
			continue;
		}
		// Out the nearest side.
		const double Left = Point.X - Wide.Min.X;
		const double Right = Wide.Max.X - Point.X;
		const double Down = Point.Y - Wide.Min.Y;
		const double Up = Wide.Max.Y - Point.Y;
		const double Least = FMath::Min(FMath::Min(Left, Right), FMath::Min(Down, Up));
		if (Least == Left)
		{
			Point.X = Wide.Min.X - 1.0;
		}
		else if (Least == Right)
		{
			Point.X = Wide.Max.X + 1.0;
		}
		else if (Least == Down)
		{
			Point.Y = Wide.Min.Y - 1.0;
		}
		else
		{
			Point.Y = Wide.Max.Y + 1.0;
		}
	}
	return FVector(Point, Location.Z);
}

FVector ARoomHouse::FindSpotBy(const FVector& Target, const FVector& From, float Distance) const
{
	// Around them, wherever's clear, in the room they're in, and nearest to walk to.
	const FName Area = AreaAt(Target);
	FVector Best = ClearOf(KeepInside(Target + (From - Target).GetSafeNormal2D() * Distance));
	float Shortest = TNumericLimits<float>::Max();
	for (int32 I = 0; I < 12; ++I)
	{
		const FVector Spot = KeepInside(Target + FRotator(0.0f, I * 30.0f, 0.0f).Vector() * Distance);
		const bool bInFurniture = FVector::Dist2D(ClearOf(Spot), Spot) > 0.5f;
		if (bInFurniture || FVector::Dist2D(Spot, Target) < Distance * 0.6f || (!Area.IsNone() && AreaAt(Spot) != Area))
		{
			continue;
		}
		const float Length = PathLength(From, Spot);
		if (Length < Shortest)
		{
			Shortest = Length;
			Best = Spot;
		}
	}
	return Best;
}

void ARoomHouse::FindDoorways(const FVector& From, const FVector& To, TArray<FVector>& OutPath) const
{
	OutPath.Reset();
	const FName Start = AreaAt(From);
	const FName Goal = AreaAt(To);
	if (Start.IsNone() || Goal.IsNone() || Start == Goal)
	{
		OutPath.Add(To);
		return;
	}

	// The fewest doorways from one area to the other.
	TMap<FName, int32> CameThrough;
	TArray<FName> Queue = {Start};
	TSet<FName> Seen = {Start};
	for (int32 Next = 0; Next < Queue.Num(); ++Next)
	{
		const FName Area = Queue[Next];
		for (int32 I = 0; I < Doors.Num(); ++I)
		{
			const FRoomDoor& Door = Doors[I];
			const FName Other = Door.From == Area ? Door.To : (Door.To == Area ? Door.From : NAME_None);
			if (!Other.IsNone() && !Seen.Contains(Other))
			{
				Seen.Add(Other);
				CameThrough.Add(Other, I);
				Queue.Add(Other);
			}
		}
	}
	if (!Seen.Contains(Goal))
	{
		OutPath.Add(To);
		return;
	}
	TArray<int32> Through;
	for (FName Area = Goal; Area != Start;)
	{
		const int32 Door = CameThrough[Area];
		Through.Insert(Door, 0);
		Area = Doors[Door].From == Area ? Doors[Door].To : Doors[Door].From;
	}

	// Through each doorway straight on, from a little way before it to a
	// little way past it.
	FName Area = Start;
	for (int32 Door : Through)
	{
		const FRoomDoor& D = Doors[Door];
		const FVector2D Normal = D.bWallAlongX ? FVector2D(0.0f, 1.0f) : FVector2D(1.0f, 0.0f);
		const FRoomArea* Here = FindArea(Area);
		const float Side = Here && ((Here->Box.GetCenter() - D.Center) | Normal) < 0.0f ? -1.0f : 1.0f;
		OutPath.Add(FVector(D.Center + Normal * Side * 100.0f, From.Z));
		OutPath.Add(FVector(D.Center - Normal * Side * 100.0f, From.Z));
		Area = D.From == Area ? D.To : D.From;
	}
	OutPath.Add(To);
}

void ARoomHouse::Grow(int32 Planter)
{
	if (Growth.IsValidIndex(Planter))
	{
		Growth[Planter] = FMath::Min(Growth[Planter] + 0.12f, 1.6f);
	}
}

FVector ARoomHouse::GetSculptureLocation(int32 Index) const
{
	return Sculptures.IsValidIndex(Index) ? Sculptures[Index]->GetComponentLocation() : FVector::ZeroVector;
}

void ARoomHouse::SpinSculpture(int32 Index, float Degrees)
{
	if (Sculptures.IsValidIndex(Index))
	{
		Sculptures[Index]->AddWorldRotation(FRotator(0.0f, Degrees, 0.0f));
	}
}

void ARoomHouse::SetParty(float Amount, double Beat, float Loudness)
{
	PartyTarget = Amount;
	PartyBeat = Beat;
	PartyLoudness = Loudness;
}
