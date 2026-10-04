//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"

class AActor;
class UMaterialInstanceDynamic;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

// The engine's basic shapes, in the house's flat colors, for things built in
// code: the props, and what the characters carry.
namespace RoomShapes
{
enum class EShape : uint8
{
	Cube,
	Sphere,
	Cylinder,
	Cone,
};

UStaticMesh* Mesh(EShape Shape);

FLinearColor Srgb(uint8 R, uint8 G, uint8 B);

// The game's flat material, in a color, matte or polished, glowing with
// `Glow` times `Emissive`.
UMaterialInstanceDynamic* Material(UObject* Outer, const FLinearColor& Color, float Roughness = 0.6f, float Glow = 0.0f,
	const FLinearColor& Emissive = FLinearColor::Black);

// A shape on `Owner`, under `Parent`, `Size` cm across (its bounding box), in
// a color, that moves, and doesn't block anything unless `bCollide`.
UStaticMeshComponent* Add(AActor* Owner, USceneComponent* Parent, EShape Shape, const FVector& Location, const FVector& Size,
	const FLinearColor& Color, float Roughness = 0.6f, const FRotator& Rotation = FRotator::ZeroRotator, bool bCollide = false);
} // namespace RoomShapes
