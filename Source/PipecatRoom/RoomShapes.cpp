//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomShapes.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
// Created by setup.ps1.
const TCHAR* const FlatPath = TEXT("/Game/Room/M_Flat.M_Flat");
} // namespace

UStaticMesh* RoomShapes::Mesh(EShape Shape)
{
	static const TCHAR* const Paths[] = {
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		TEXT("/Engine/BasicShapes/Sphere.Sphere"),
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
		TEXT("/Engine/BasicShapes/Cone.Cone"),
	};
	return LoadObject<UStaticMesh>(nullptr, Paths[static_cast<int32>(Shape)]);
}

FLinearColor RoomShapes::Srgb(uint8 R, uint8 G, uint8 B)
{
	return FLinearColor::FromSRGBColor(FColor(R, G, B));
}

UMaterialInstanceDynamic* RoomShapes::Material(
	UObject* Outer, const FLinearColor& Color, float Roughness, float Glow, const FLinearColor& Emissive)
{
	UMaterialInterface* Flat = LoadObject<UMaterialInterface>(nullptr, FlatPath);
	UMaterialInstanceDynamic* Instance =
		UMaterialInstanceDynamic::Create(Flat ? Flat : UMaterial::GetDefaultMaterial(MD_Surface), Outer);
	Instance->SetVectorParameterValue(TEXT("Color"), Color);
	Instance->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	Instance->SetVectorParameterValue(TEXT("Emissive"), Emissive);
	Instance->SetScalarParameterValue(TEXT("EmissiveStrength"), Glow);
	return Instance;
}

UStaticMeshComponent* RoomShapes::Add(AActor* Owner, USceneComponent* Parent, EShape Shape, const FVector& Location,
	const FVector& Size, const FLinearColor& Color, float Roughness, const FRotator& Rotation, bool bCollide)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetStaticMesh(Mesh(Shape));
	Component->SetupAttachment(Parent ? Parent : Owner->GetRootComponent());
	Component->SetRelativeTransform(FTransform(Rotation, Location, Size / 100.0f));
	Component->SetMaterial(0, Material(Owner, Color, Roughness));
	Component->SetCollisionProfileName(bCollide ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	Component->RegisterComponent();
	return Component;
}
