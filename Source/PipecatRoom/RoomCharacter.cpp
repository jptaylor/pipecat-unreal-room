//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomCharacter.h"

#include "RoomCharacterAnim.h"
#include "RoomHouse.h"
#include "RoomItem.h"
#include "RoomMusic.h"

#include "Animation/BlendSpace.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoomCharacter, Log, All);

namespace
{
// The template's mannequins and their animations, copied by setup.ps1, and
// the game's material, created by it.
const TCHAR* const MannyPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
const TCHAR* const QuinnPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple");
const TCHAR* const LocomotionPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run.BS_Idle_Walk_Run");
const TCHAR* const FlatPath = TEXT("/Game/Room/M_Flat.M_Flat");

// How fast they walk, and run when they're far behind, in cm/s.
const float WalkSpeed = 165.0f;
const float RunSpeed = 360.0f;
const float RunBeyond = 900.0f;

// Following, they keep this far behind the player, and set off again once
// they're further than that.
const float FollowNear = 140.0f;
const float FollowFar = 300.0f;

// How far their head turns, in degrees, and how far to the side what they
// face gets before their body turns too, at this many degrees a second.
const float MaxLookYaw = 75.0f;
const float MinLookPitch = -35.0f;
const float MaxLookPitch = 40.0f;
const float TurnFrom = 45.0f;
const float TurnUntil = 8.0f;
const float TurnSpeed = 150.0f;
// They notice the player within this many cm.
const float NoticeDistance = 650.0f;

// The mesh faces +Y, with its right to -X.
const FVector MeshForward(0.0f, 1.0f, 0.0f);
const FVector MeshRight(-1.0f, 0.0f, 0.0f);
} // namespace

FRoomStep FRoomStep::WalkTo(const FVector& Where, float Yaw)
{
	FRoomStep Step;
	Step.Kind = EKind::Walk;
	Step.Where = Where;
	Step.Yaw = Yaw;
	return Step;
}

FRoomStep FRoomStep::BusyWith(ERoomActivity Activity, float Seconds)
{
	FRoomStep Step;
	Step.Kind = EKind::Busy;
	Step.Activity = Activity;
	Step.Seconds = Seconds;
	return Step;
}

FRoomStep FRoomStep::TakeItem(ERoomItem Item, const FLinearColor& Tint, const FString& Name)
{
	FRoomStep Step;
	Step.Kind = EKind::Take;
	Step.Item = Item;
	Step.Tint = Tint;
	Step.ItemName = Name;
	return Step;
}

FRoomStep FRoomStep::GiveTo(AActor* To)
{
	FRoomStep Step;
	Step.Kind = EKind::Give;
	Step.To = To;
	return Step;
}

FRoomStep FRoomStep::Make(ERoomGesture Gesture)
{
	FRoomStep Step;
	Step.Kind = EKind::Gesture;
	Step.Gesture = Gesture;
	return Step;
}

FRoomStep FRoomStep::Call(TFunction<void()> Then)
{
	FRoomStep Step;
	Step.Kind = EKind::Call;
	Step.Then = MoveTemp(Then);
	return Step;
}

ARoomCharacter::ARoomCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	bUseControllerRotationYaw = false;

	GetCapsuleComponent()->InitCapsuleSize(34.0f, 90.0f);
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

	USkeletalMeshComponent* Body = GetMesh();
	Body->SetRelativeLocationAndRotation(FVector(0.0f, 0.0f, -90.0f), FRotator(0.0f, -90.0f, 0.0f));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Posed even out of sight, so they're looking the right way when the
	// player turns to them.
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	// They walk without a controller, steered by the house's paths.
	Movement->bRunPhysicsWithNoController = true;
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0f, 300.0f, 0.0f);
	Movement->MaxWalkSpeed = WalkSpeed;
	Movement->MaxAcceleration = 700.0f;
	Movement->BrakingDecelerationWalking = 900.0f;
	Movement->bUseRVOAvoidance = true;
	Movement->AvoidanceConsiderationRadius = 260.0f;
	Movement->AvoidanceWeight = 0.5f;

	// A soft glow in their color as they speak.
	Aura = CreateDefaultSubobject<UPointLightComponent>(TEXT("Aura"));
	Aura->SetupAttachment(GetCapsuleComponent());
	Aura->SetRelativeLocation(FVector(30.0f, 0.0f, 40.0f));
	Aura->SetIntensityUnits(ELightUnits::Candelas);
	Aura->SetIntensity(0.0f);
	Aura->SetAttenuationRadius(450.0f);
	Aura->SetSourceRadius(20.0f);
	Aura->SetCastShadows(false);
}

void ARoomCharacter::Setup(const FRoomCast& InCast, ARoomHouse* InHouse)
{
	Info = InCast;
	House = InHouse;
	Seed = static_cast<float>(GetTypeHash(Info.Id) % 1000) / 1000.0f;
}

void ARoomCharacter::BeginPlay()
{
	Super::BeginPlay();

	USkeletalMesh* Asset = LoadObject<USkeletalMesh>(nullptr, Info.Body == TEXT("quinn") ? QuinnPath : MannyPath);
	UBlendSpace* Locomotion = LoadObject<UBlendSpace>(nullptr, LocomotionPath);
	if (!Asset || !Locomotion)
	{
		UE_LOG(LogRoomCharacter, Warning, TEXT("No mannequin for %s: run setup.ps1"), *Info.Id);
		return;
	}
	USkeletalMeshComponent* Body = GetMesh();
	Body->SetSkeletalMeshAsset(Asset);
	SetActorScale3D(FVector(Info.Height));

	// In their own color, a flat one, like everything in the house.
	if (UMaterialInterface* Flat = LoadObject<UMaterialInterface>(nullptr, FlatPath))
	{
		Material = UMaterialInstanceDynamic::Create(Flat, this);
		Material->SetVectorParameterValue(TEXT("Color"), Info.Color);
		Material->SetScalarParameterValue(TEXT("Roughness"), 0.5f);
		Material->SetVectorParameterValue(TEXT("RimColor"), FMath::Lerp(Info.Color, FLinearColor::White, 0.35f));
		Material->SetScalarParameterValue(TEXT("Rim"), 0.0f);
		for (int32 Index = 0; Index < Body->GetNumMaterials(); ++Index)
		{
			Body->SetMaterial(Index, Material);
		}
	}
	Aura->SetLightColor(Info.Color);

	// The template's locomotion is by direction and speed: which axis is which.
	SpeedAxis = 0;
	DirectionAxis = INDEX_NONE;
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		const FString Name = Locomotion->GetBlendParameter(Axis).DisplayName;
		if (Name.Contains(TEXT("Speed")))
		{
			SpeedAxis = Axis;
		}
		else if (Name.Contains(TEXT("Direction")))
		{
			DirectionAxis = Axis;
		}
	}

	Body->SetAnimInstanceClass(URoomCharacterAnim::StaticClass());
	if (URoomCharacterAnim* Anim = GetAnim())
	{
		Anim->SetAnimationAsset(Locomotion, true, 1.0f);
		Anim->SetPlaying(true);
		Anim->SetPosition(Seed * 3.0f, false);
	}
	// They decide where to look before they're posed, every frame.
	Body->AddTickPrerequisiteActor(this);

	if (House.IsValid())
	{
		RestYaw = House->GetHome(Info.Home).Yaw;
		SetActorRotation(FRotator(0.0f, RestYaw, 0.0f));
	}
}

URoomCharacterAnim* ARoomCharacter::GetAnim() const
{
	return Cast<URoomCharacterAnim>(GetMesh()->GetAnimInstance());
}

USceneComponent* ARoomCharacter::GetVoiceParent() const
{
	return GetMesh();
}

FVector ARoomCharacter::GetHeadLocation() const
{
	const USkeletalMeshComponent* Body = GetMesh();
	return Body && Body->DoesSocketExist(TEXT("head")) ? Body->GetSocketLocation(TEXT("head"))
													  : GetActorLocation() + FVector(0.0f, 0.0f, 70.0f);
}

//
// What they're told
//

void ARoomCharacter::SetIntent(ERoomIntent InIntent, FName Area)
{
	// Asked to go somewhere, they leave what they were doing.
	StopDoing();
	if (InIntent != ERoomIntent::Wait)
	{
		SetDancing(false);
	}
	Intent = InIntent;
	GoArea = Area;
	Replan = 0.0f;
	Stop();
	if (Intent == ERoomIntent::Wait)
	{
		RestYaw = GetActorRotation().Yaw;
	}
	UE_LOG(LogRoomCharacter, Log, TEXT("%s: %s%s%s"), *Info.Id, GetIntentName(), Area.IsNone() ? TEXT("") : TEXT(" to "),
		Area.IsNone() ? TEXT("") : *Area.ToString());
}

const TCHAR* ARoomCharacter::GetIntentName() const
{
	switch (Intent)
	{
	case ERoomIntent::Home:
		return TEXT("home");
	case ERoomIntent::Wait:
		return TEXT("wait");
	case ERoomIntent::Follow:
		return TEXT("follow");
	case ERoomIntent::Come:
		return TEXT("come");
	case ERoomIntent::Go:
		return TEXT("go");
	}
	return TEXT("home");
}

void ARoomCharacter::Do(TArray<FRoomStep> InSteps, const FString& InDoing, TFunction<void()> InOnEnd)
{
	StopDoing();
	SetDancing(false);
	Steps = MoveTemp(InSteps);
	Doing = InDoing;
	OnEnd = MoveTemp(InOnEnd);
	StepTime = 0.0f;
	bStepStarted = false;
	UE_LOG(LogRoomCharacter, Log, TEXT("%s: %s"), *Info.Id, *Doing);
}

void ARoomCharacter::StopDoing()
{
	const bool bWasBusy = !Steps.IsEmpty();
	if (bWasBusy && Steps[0].Kind == FRoomStep::EKind::Give)
	{
		TalkTo(nullptr);
	}
	Replan = 0.0f;
	if (bWasBusy)
	{
		UE_LOG(LogRoomCharacter, Log, TEXT("%s: stopped %s"), *Info.Id, *Doing);
	}
	Steps.Reset();
	Doing.Reset();
	Activity = ERoomActivity::None;
	bFacingFixed = false;
	if (bWasBusy)
	{
		Stop();
	}
	if (OnEnd)
	{
		TFunction<void()> End = MoveTemp(OnEnd);
		OnEnd = nullptr;
		End();
	}
}

void ARoomCharacter::NextStep()
{
	if (!Steps.IsEmpty())
	{
		Steps.RemoveAt(0);
	}
	StepTime = 0.0f;
	bStepStarted = false;
	if (Steps.IsEmpty())
	{
		Replan = 0.0f;
		UE_LOG(LogRoomCharacter, Log, TEXT("%s: done %s"), *Info.Id, *Doing);
		Doing.Reset();
		Activity = ERoomActivity::None;
		bFacingFixed = false;
		if (OnEnd)
		{
			TFunction<void()> End = MoveTemp(OnEnd);
			OnEnd = nullptr;
			End();
		}
	}
}

void ARoomCharacter::SetDancing(bool bOn, bool bAuto)
{
	if (bOn && !bDancing)
	{
		OwnBeat = Time * 110.0 / 60.0;
	}
	bDancing = bOn;
	bAutoDance = bOn && bAuto;
}

void ARoomCharacter::SetBeat(double InBeat, float Strength)
{
	Beat = InBeat;
	BeatStrength = Strength;
}

ERoomItem ARoomCharacter::GetHeldKind() const
{
	return Held ? Held->GetKind() : ERoomItem::None;
}

void ARoomCharacter::Hold(ARoomItem* Item)
{
	if (Held && Held != Item)
	{
		Held->Vanish();
	}
	Held = Item;
	if (Held)
	{
		Held->PutIn(GetMesh());
	}
}

ARoomItem* ARoomCharacter::Release()
{
	ARoomItem* Item = Held;
	Held = nullptr;
	return Item;
}

void ARoomCharacter::UpdateJob(float DeltaSeconds)
{
	if (Steps.IsEmpty())
	{
		return;
	}
	FRoomStep& Step = Steps[0];
	StepTime += DeltaSeconds;
	const bool bFirst = !bStepStarted;
	bStepStarted = true;
	switch (Step.Kind)
	{
	case FRoomStep::EKind::Walk:
		if (bFirst)
		{
			bFacingFixed = false;
			MoveTo(House.IsValid() ? House->KeepInside(Step.Where) : Step.Where, 30.0f);
		}
		else if (!bWalking || StepTime > 40.0f)
		{
			// There, or as near as they'll get: they face the way they'll work.
			Stop();
			RestYaw = Step.Yaw;
			bFacingFixed = true;
			NextStep();
		}
		break;
	case FRoomStep::EKind::Busy:
		Activity = Step.Activity;
		bFacingFixed = true;
		if (Step.Seconds > 0.0f && StepTime >= Step.Seconds)
		{
			Activity = ERoomActivity::None;
			NextStep();
		}
		break;
	case FRoomStep::EKind::Take:
		Hold(ARoomItem::Make(GetWorld(), Step.Item, GetMesh(), Step.Tint, Step.ItemName));
		URoomMusic::PlaySound(GetWorld(), GetActorLocation() + FVector(0.0f, 0.0f, 20.0f), ERoomSound::Pop, 0.5f);
		NextStep();
		break;
	case FRoomStep::EKind::Give:
	{
		AActor* To = Step.To.Get();
		if (!To || !Held)
		{
			NextStep();
			break;
		}
		const FVector Here = GetActorLocation();
		const float Away = FVector::Dist2D(Here, To->GetActorLocation());
		bFacingFixed = false;
		if (Away > 150.0f && StepTime < 30.0f)
		{
			// Up to them, keeping up as they move.
			if (!bWalking || (Replan <= 0.0f && FVector::Dist2D(To->GetActorLocation(), GiveFrom) > 80.0f))
			{
				Replan = 0.5f;
				GiveFrom = To->GetActorLocation();
				const FVector Spot = House.IsValid() ? House->FindSpotBy(GiveFrom, Here, 110.0f)
													 : GiveFrom + (Here - GiveFrom).GetSafeNormal2D() * 110.0f;
				MoveTo(Spot, 30.0f);
			}
			break;
		}
		if (Away > 250.0f)
		{
			// They couldn't get to them: they keep it.
			UE_LOG(LogRoomCharacter, Log, TEXT("%s: couldn't reach %s to hand over %s"), *Info.Id, *To->GetName(), *Held->GetName());
			TalkTo(nullptr);
			NextStep();
			break;
		}
		// Then they hold it out, and it's taken.
		if (bWalking)
		{
			Stop();
		}
		TalkTo(To);
		if (CurrentGesture != ERoomGesture::Offer)
		{
			Gesture(ERoomGesture::Offer);
		}
		if (GestureTime > 0.8f)
		{
			if (OnGive)
			{
				OnGive(this, To);
			}
			TalkTo(nullptr);
			NextStep();
		}
		break;
	}
	case FRoomStep::EKind::Gesture:
		if (bFirst)
		{
			Gesture(Step.Gesture);
		}
		if (StepTime >= RoomTypes::GestureSeconds(Step.Gesture))
		{
			NextStep();
		}
		break;
	case FRoomStep::EKind::Call:
	{
		TFunction<void()> Then = MoveTemp(Step.Then);
		NextStep();
		if (Then)
		{
			Then();
		}
		break;
	}
	}
}

void ARoomCharacter::SetSlot(int32 InSlot, int32 InCount)
{
	Slot = InSlot;
	SlotCount = FMath::Max(InCount, 1);
}

void ARoomCharacter::SetMood(ERoomMood InMood, float Strength, float Seconds)
{
	TargetMood = InMood;
	MoodStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
	MoodLeft = Seconds;
}

void ARoomCharacter::Gesture(ERoomGesture InGesture, float Strength)
{
	if (InGesture == ERoomGesture::None)
	{
		return;
	}
	CurrentGesture = InGesture;
	GestureTime = 0.0f;
	GestureStrength = FMath::Clamp(Strength, 0.3f, 1.0f);
}

void ARoomCharacter::LookAt(AActor* Target, float Seconds)
{
	LookTarget = Target;
	LookLeft = Seconds;
}

void ARoomCharacter::TalkTo(AActor* Target)
{
	Addressee = Target;
}

void ARoomCharacter::SetVoiceLevel(float Level)
{
	Voice = FMath::Clamp(Level, 0.0f, 1.0f);
}

void ARoomCharacter::SetHearsPlayer(bool bHears)
{
	if (bHears && !bHearsPlayer)
	{
		// The voice reaching them catches their attention.
		Attention = 1.0f;
	}
	bHearsPlayer = bHears;
}

void ARoomCharacter::SetListening(AActor* InSpeaker)
{
	Speaker = InSpeaker;
}

//
// Each frame
//

void ARoomCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	UpdateMovement(DeltaSeconds);
	UpdateFacing(DeltaSeconds);
	UpdateLook(DeltaSeconds);
	UpdatePose(DeltaSeconds);
	UpdateGlow(DeltaSeconds);
}

FVector ARoomCharacter::SlotNearPlayer(bool bBehind) const
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return GetActorLocation();
	}
	// Following, they keep behind and to the left of the player, out of the
	// camera's way, which is over the player's right shoulder.
	const float Spread = bBehind ? 60.0f : 110.0f;
	const float Fraction = SlotCount > 1 ? static_cast<float>(Slot) / (SlotCount - 1) - 0.5f : 0.0f;
	const float Angle = (bBehind ? 240.0f : 0.0f) + Spread * Fraction;
	const float Distance = bBehind ? 175.0f : 165.0f;
	const FVector Direction = FRotator(0.0f, Player->GetActorRotation().Yaw + Angle, 0.0f).Vector();
	FVector Spot = Player->GetActorLocation() + Direction * Distance;
	Spot.Z = GetActorLocation().Z;
	if (!House.IsValid())
	{
		return Spot;
	}
	// In the player's room, clear of the furniture: if the place behind or in
	// front of them is through a wall (they're by one, facing it), or in
	// something, the nearest clear place around them instead.
	const FVector Kept = House->KeepInside(Spot);
	if (House->AreaAt(Kept) != House->AreaAt(Player->GetActorLocation()) || FVector::Dist2D(House->ClearOf(Kept), Kept) > 0.5f)
	{
		return House->FindSpotBy(Player->GetActorLocation(), GetActorLocation(), Distance);
	}
	return Kept;
}

void ARoomCharacter::MoveTo(const FVector& InGoal, float InAccept)
{
	Goal = InGoal;
	Accept = InAccept;
	if (House.IsValid())
	{
		// Around the furniture, and the player and anyone else standing about.
		TArray<FBox2D> People;
		for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
		{
			if (*It != this && It->GetVelocity().Size2D() < 40.0f)
			{
				People.Add(FBox2D(FVector2D(It->GetActorLocation()), FVector2D(It->GetActorLocation())).ExpandBy(34.0f));
			}
		}
		House->FindPath(GetActorLocation(), Goal, Path, People);
	}
	else
	{
		Path = {Goal};
	}
	PathIndex = 0;
	bWalking = true;
	Stuck = 0.0f;
	UE_LOG(LogRoomCharacter, Verbose, TEXT("%s: walking from (%.0f, %.0f) to (%.0f, %.0f), by %d points"), *Info.Id, GetActorLocation().X,
		GetActorLocation().Y, Goal.X, Goal.Y, Path.Num());
}

void ARoomCharacter::Stop()
{
	bWalking = false;
	Path.Reset();
	PathIndex = 0;
}

void ARoomCharacter::UpdateMovement(float DeltaSeconds)
{
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const FVector Here = GetActorLocation();
	Replan -= DeltaSeconds;

	// What they've been asked to do comes first; then where they mean to be.
	if (!Steps.IsEmpty())
	{
		UpdateJob(DeltaSeconds);
	}
	else switch (Intent)
	{
	case ERoomIntent::Home:
		if (House.IsValid() && !bWalking)
		{
			const FRoomSpot Home = House->GetHome(Info.Home);
			if (FVector::Dist2D(Here, Home.Location) > 60.0f)
			{
				MoveTo(Home.Location, 35.0f);
			}
			else
			{
				RestYaw = Home.Yaw;
			}
		}
		break;
	case ERoomIntent::Follow:
		if (Player && Replan <= 0.0f)
		{
			Replan = 0.4f;
			const FVector Spot = SlotNearPlayer(true);
			const float Away = FVector::Dist2D(Here, Player->GetActorLocation());
			if (!bWalking && Away > FollowFar)
			{
				MoveTo(Spot, FollowNear * 0.5f);
			}
			else if (bWalking && FVector::Dist2D(Goal, Spot) > 80.0f)
			{
				MoveTo(Spot, FollowNear * 0.5f);
			}
			if (bWalking && Away < FollowNear)
			{
				Stop();
			}
		}
		break;
	case ERoomIntent::Come:
		if (Player && Replan <= 0.0f)
		{
			Replan = 0.6f;
			// Already beside them (having just handed them something, say): there.
			if (!bWalking && FVector::Dist2D(Here, Player->GetActorLocation()) < 220.0f)
			{
				Intent = ERoomIntent::Wait;
				RestYaw = GetActorRotation().Yaw;
				break;
			}
			const FVector Spot = SlotNearPlayer(false);
			if (!bWalking || FVector::Dist2D(Goal, Spot) > 120.0f)
			{
				MoveTo(Spot, 45.0f);
			}
		}
		break;
	case ERoomIntent::Go:
		if (House.IsValid() && !bWalking && Replan <= 0.0f)
		{
			Replan = 1000.0f;
			// Their own room: their own place in it. The room the player's in:
			// in front of them, where they can see them. Someone else's: near
			// the middle of it, each of those sent a little apart, and not
			// where whoever lives there stands.
			FVector Spot = House->GetHome(GoArea).Location;
			if (GoArea != Info.Home && Player && House->AreaAt(Player->GetActorLocation()) == GoArea)
			{
				Spot = SlotNearPlayer(false);
			}
			else if (GoArea != Info.Home)
			{
				const FRoomArea* Area = House->FindArea(GoArea);
				const FVector Hub = Area ? FVector(Area->Hub, 0.0f) : Spot;
				const float Around = 360.0f * Seed + 110.0f * Slot;
				Spot = Hub + FRotator(0.0f, Around, 0.0f).Vector() * 110.0f;
			}
			MoveTo(House->KeepInside(Spot), 45.0f);
		}
		break;
	case ERoomIntent::Wait:
		break;
	}

	if (!bWalking || Path.IsEmpty())
	{
		return;
	}

	// Along the way, a doorway at a time.
	FVector Waypoint = Path[PathIndex];
	FVector To = Waypoint - Here;
	To.Z = 0.0f;
	float Distance = To.Size();
	const bool bLast = PathIndex == Path.Num() - 1;
	if (!bLast && Distance < 45.0f)
	{
		++PathIndex;
		Waypoint = Path[PathIndex];
		To = Waypoint - Here;
		To.Z = 0.0f;
		Distance = To.Size();
	}
	if (PathIndex == Path.Num() - 1 && Distance < Accept)
	{
		Stop();
		if (Steps.IsEmpty() && (Intent == ERoomIntent::Come || Intent == ERoomIntent::Go))
		{
			// There: they wait, facing the player or the room.
			Intent = ERoomIntent::Wait;
		}
		RestYaw = GetActorRotation().Yaw;
		return;
	}

	// Running when they're far behind, slowing as they arrive, and keeping
	// out of the player's way.
	float Remaining = Distance;
	for (int32 I = PathIndex + 1; I < Path.Num(); ++I)
	{
		Remaining += FVector::Dist2D(Path[I - 1], Path[I]);
	}
	GetCharacterMovement()->MaxWalkSpeed = Remaining > RunBeyond ? RunSpeed : WalkSpeed;
	FVector Direction = To.GetSafeNormal();
	if (Player)
	{
		FVector Away = Here - Player->GetActorLocation();
		Away.Z = 0.0f;
		const float Close = Away.Size();
		if (Close < 110.0f && Close > 1.0f)
		{
			Direction = (Direction + Away / Close * (1.0f - Close / 110.0f) * 1.5f).GetSafeNormal();
		}
	}
	const float Scale = PathIndex == Path.Num() - 1 ? FMath::Clamp(Distance / 150.0f, 0.35f, 1.0f) : 1.0f;
	AddMovementInput(Direction, Scale);

	// Stuck against something: the way worked out again, and if they're
	// still stuck, a few steps aside first.
	if (GetVelocity().Size2D() < 15.0f)
	{
		Stuck += DeltaSeconds;
		if (Stuck > 1.2f)
		{
			Stuck = 0.0f;
			StuckTimes = FVector::Dist2D(Here, StuckAt) < 40.0f ? StuckTimes + 1 : 1;
			StuckAt = Here;
			if (PathIndex == Path.Num() - 1 && FVector::Dist2D(Here, Goal) < 130.0f)
			{
				// As near as they'll get (someone's in the way, say).
				Stop();
				if (Steps.IsEmpty() && (Intent == ERoomIntent::Come || Intent == ERoomIntent::Go))
				{
					Intent = ERoomIntent::Wait;
				}
				RestYaw = GetActorRotation().Yaw;
				return;
			}
			const FVector Target = Goal;
			MoveTo(Target, Accept);
			if (StuckTimes >= 2 && House.IsValid())
			{
				const FVector Aside = FVector::CrossProduct(Direction, FVector::UpVector) * (FMath::RandBool() ? 1.0f : -1.0f);
				const FVector Step = House->ClearOf(House->KeepInside(Here + (Aside - Direction * 0.5f).GetSafeNormal2D() * 120.0f));
				Path.Insert(Step, 0);
			}
		}
	}
	else
	{
		Stuck = 0.0f;
	}
}

void ARoomCharacter::UpdateFacing(float DeltaSeconds)
{
	if (bWalking)
	{
		return;
	}
	// They face who they're talking to, or listening to, or the player when
	// they're near, or the way they were.
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const AActor* Face = Addressee.IsValid() ? Addressee.Get() : (Speaker.IsValid() ? Speaker.Get() : nullptr);
	if (!Face && Player && FVector::Dist2D(Player->GetActorLocation(), GetActorLocation()) < NoticeDistance)
	{
		Face = Player;
	}
	float Want = RestYaw;
	if (Face && Face != this && !bFacingFixed)
	{
		Want = (Face->GetActorLocation() - GetActorLocation()).Rotation().Yaw;
	}
	const float Off = FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, Want);
	if (FMath::Abs(Off) > TurnFrom)
	{
		bTurning = true;
	}
	else if (FMath::Abs(Off) < TurnUntil)
	{
		bTurning = false;
	}
	if (bTurning)
	{
		AddActorWorldRotation(FRotator(0.0f, FMath::Sign(Off) * FMath::Min(FMath::Abs(Off), TurnSpeed * DeltaSeconds), 0.0f));
	}
}

void ARoomCharacter::UpdateLook(float DeltaSeconds)
{
	LookLeft -= DeltaSeconds;
	const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const AActor* Target = nullptr;
	if (LookLeft > 0.0f && LookTarget.IsValid())
	{
		Target = LookTarget.Get();
	}
	else if (Addressee.IsValid())
	{
		Target = Addressee.Get();
	}
	else if (Speaker.IsValid())
	{
		Target = Speaker.Get();
	}
	else if (Player && FVector::Dist(Player->GetActorLocation(), GetActorLocation()) < NoticeDistance * 1.5f)
	{
		Target = Player;
	}

	// With nobody to look at, they glance about now and then.
	GlanceIn -= DeltaSeconds;
	if (GlanceIn <= 0.0f)
	{
		GlanceIn = FMath::FRandRange(3.0f, 8.0f);
		const bool bAhead = FMath::RandBool();
		GlanceYaw = bAhead ? 0.0f : FMath::FRandRange(-45.0f, 45.0f);
		GlancePitch = bAhead ? 0.0f : FMath::FRandRange(-15.0f, 8.0f);
	}
	float Yaw = bWalking ? 0.0f : GlanceYaw;
	float Pitch = bWalking ? 0.0f : GlancePitch;
	if (Target && Target != this && !bWalking)
	{
		const FVector Eyes = GetHeadLocation();
		const ARoomCharacter* Other = Cast<ARoomCharacter>(Target);
		const APawn* Pawn = Cast<APawn>(Target);
		const FVector At = Other ? Other->GetHeadLocation() : (Pawn ? Pawn->GetPawnViewLocation() : Target->GetActorLocation());
		const FVector Local = GetMesh()->GetComponentTransform().InverseTransformVectorNoScale(At - Eyes);
		const float Ahead = Local | MeshForward;
		const float Aside = Local | MeshRight;
		Yaw = FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan2(Aside, Ahead)), -MaxLookYaw, MaxLookYaw);
		Pitch = FMath::Clamp(
			FMath::RadiansToDegrees(FMath::Atan2(Local.Z, FMath::Sqrt(Ahead * Ahead + Aside * Aside))), MinLookPitch, MaxLookPitch);
	}
	LookYaw = FMath::FInterpTo(LookYaw, Yaw, DeltaSeconds, 5.0f);
	LookPitch = FMath::FInterpTo(LookPitch, Pitch, DeltaSeconds, 5.0f);
}

void ARoomCharacter::UpdatePose(float DeltaSeconds)
{
	URoomCharacterAnim* Anim = GetAnim();
	if (!Anim)
	{
		return;
	}

	// Their mood eases in, lasts, and eases out; one at a time, with the
	// last fading as the next comes.
	MoodLeft -= DeltaSeconds;
	const int32 Want = MoodLeft > 0.0f ? static_cast<int32>(TargetMood) : 0;
	for (int32 M = 1; M < static_cast<int32>(ERoomMood::Count); ++M)
	{
		const float Target = M == Want ? MoodStrength : 0.0f;
		Mood[M] = FMath::FInterpTo(Mood[M], Target, DeltaSeconds, Target > Mood[M] ? 3.0f : 1.6f);
	}
	if (CurrentGesture != ERoomGesture::None)
	{
		GestureTime += DeltaSeconds;
		if (GestureTime > RoomTypes::GestureSeconds(CurrentGesture))
		{
			CurrentGesture = ERoomGesture::None;
		}
	}

	// Talking eases in with their voice, and lingers a moment between words.
	if (Voice > 0.03f)
	{
		TalkHold = 1.0f;
	}
	TalkHold -= DeltaSeconds;
	Talk = FMath::FInterpTo(Talk, TalkHold > 0.0f ? 1.0f : 0.0f, DeltaSeconds, 3.0f);
	Listen = FMath::FInterpTo(Listen, Speaker.IsValid() && Talk < 0.5f ? 1.0f : 0.0f, DeltaSeconds, 2.0f);
	Attention = FMath::FInterpTo(Attention, 0.0f, DeltaSeconds, 1.2f);

	FRoomPoseInput& In = Anim->Input;
	In.Time = Time;
	In.Seed = Seed;
	for (int32 M = 0; M < static_cast<int32>(ERoomMood::Count); ++M)
	{
		In.Mood[M] = Mood[M];
	}
	// Thinking about what to say, they look it.
	if (bThinking && Talk < 0.2f)
	{
		In.Mood[static_cast<int32>(ERoomMood::Thinking)] = FMath::Max(In.Mood[static_cast<int32>(ERoomMood::Thinking)], 0.35f);
	}
	In.Gesture = CurrentGesture;
	In.GestureTime = GestureTime;
	In.GestureStrength = GestureStrength;
	const AActor* Toward = Addressee.IsValid() ? Addressee.Get() : (LookTarget.IsValid() ? LookTarget.Get() : nullptr);
	if (!Toward)
	{
		Toward = UGameplayStatics::GetPlayerPawn(this, 0);
	}
	if (Toward)
	{
		const FVector Shoulder = GetMesh()->DoesSocketExist(TEXT("upperarm_r")) ? GetMesh()->GetSocketLocation(TEXT("upperarm_r"))
																				: GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);
		const FVector Direction = (Toward->GetActorLocation() + FVector(0.0f, 0.0f, 40.0f)) - Shoulder;
		In.GestureDirection = GetMesh()->GetComponentTransform().InverseTransformVectorNoScale(Direction).GetSafeNormal();
	}
	In.Talk = Talk;
	In.Voice = Voice;
	In.Listen = Listen;
	const float Speed = GetVelocity().Size2D();
	In.Moving = FMath::Clamp(Speed / WalkSpeed, 0.0f, 1.0f);
	In.Attention = Attention;

	float YawSin, YawCos, PitchSin, PitchCos;
	FMath::SinCos(&YawSin, &YawCos, FMath::DegreesToRadians(LookYaw));
	FMath::SinCos(&PitchSin, &PitchCos, FMath::DegreesToRadians(LookPitch));
	const FVector Direction = PitchCos * (YawCos * MeshForward + YawSin * MeshRight) + PitchSin * FVector::UpVector;
	In.Look = FQuat::FindBetweenNormals(MeshForward, Direction.GetSafeNormal());

	// Busy with their hands, dancing, holding something.
	if (Activity != ERoomActivity::None)
	{
		In.Activity = Activity;
	}
	ActivityAmount = FMath::FInterpTo(ActivityAmount, Activity != ERoomActivity::None && !bWalking ? 1.0f : 0.0f, DeltaSeconds, 3.0f);
	In.ActivityAmount = ActivityAmount;
	if (ActivityAmount < 0.01f)
	{
		In.Activity = ERoomActivity::None;
	}
	OwnBeat += DeltaSeconds * 110.0 / 60.0;
	In.Beat = BeatStrength > 0.15f ? Beat : OwnBeat;
	DanceAmount = FMath::FInterpTo(DanceAmount, bDancing && !bWalking ? 1.0f : 0.0f, DeltaSeconds, 2.5f);
	In.Dance = DanceAmount * (1.0f - 0.5f * Talk);
	HoldAmount = FMath::FInterpTo(HoldAmount, Held ? 1.0f : 0.0f, DeltaSeconds, 4.0f);
	In.Hold = HoldAmount;

	// Walking or running, forwards mostly, as fast as they're going.
	FVector Position = FVector::ZeroVector;
	Position[SpeedAxis] = Speed;
	if (DirectionAxis != INDEX_NONE && Speed > 5.0f)
	{
		Position[DirectionAxis] = FRotator::NormalizeAxis(GetVelocity().Rotation().Yaw - GetActorRotation().Yaw);
	}
	Anim->SetBlendSpacePosition(Position);
}

void ARoomCharacter::UpdateGlow(float DeltaSeconds)
{
	// Their edges light up as the player's voice reaches them, and with their
	// own voice as they speak, and their color spills onto what's near.
	Hearing = FMath::FInterpTo(Hearing, bHearsPlayer ? 1.0f : 0.0f, DeltaSeconds, bHearsPlayer ? 6.0f : 1.5f);
	const float Rim = 0.9f * Hearing + 0.6f * Attention + 2.2f * Voice * Talk;
	if (Material)
	{
		Material->SetScalarParameterValue(TEXT("Rim"), Rim);
	}
	Aura->SetIntensity(6.0f * Voice * Talk + 0.8f * Hearing);

	// Out of sight while the camera's inside them, as they pass right by it.
	if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		const FVector Eye = Camera->GetCameraLocation();
		const FVector Feet = GetActorLocation() - FVector(0.0f, 0.0f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		const bool bInside = FVector::Dist2D(Eye, Feet) < 50.0f && Eye.Z > Feet.Z && Eye.Z < Feet.Z + 200.0f;
		if (bInside == GetMesh()->IsVisible())
		{
			GetMesh()->SetVisibility(!bInside, true);
		}
	}
}
