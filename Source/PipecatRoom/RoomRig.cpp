//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomRig.h"

#include "BonePose.h"
#include "TwoBoneIK.h"

namespace
{
// The mesh's axes: the mannequin faces +Y, with its left to +X.
const FVector Ahead(0.0f, 1.0f, 0.0f);
const FVector Up(0.0f, 0.0f, 1.0f);
const FVector Left(1.0f, 0.0f, 0.0f);
// Its right: turning about it tips the head forward, or leans it over.
const FVector Right(-1.0f, 0.0f, 0.0f);

float Ease(float X)
{
	X = FMath::Clamp(X, 0.0f, 1.0f);
	return X * X * (3.0f - 2.0f * X);
}

// From 0 up to 1 and back down, over a gesture `Length` seconds long.
float Envelope(float Time, float Length)
{
	if (Length <= 0.0f || Time < 0.0f || Time > Length)
	{
		return 0.0f;
	}
	const float T = Time / Length;
	return Ease(T / 0.22f) * (1.0f - Ease((T - 0.7f) / 0.3f));
}

float Sine(float Time, float Frequency, float Phase = 0.0f)
{
	return FMath::Sin(UE_TWO_PI * (Frequency * Time + Phase));
}

// Smooth noise, about -1 to 1.
float Noise(float Time, float Seed)
{
	return 0.5f * FMath::Sin(Time * 1.31f + Seed * 7.13f) + 0.3f * FMath::Sin(Time * 2.87f + Seed * 3.71f)
		+ 0.2f * FMath::Sin(Time * 5.27f + Seed * 1.93f);
}

FQuat Turn(const FVector& Axis, float Degrees)
{
	return FQuat(Axis, FMath::DegreesToRadians(Degrees));
}

// What the arms do in a pose.
enum class EArms : uint8
{
	None,
	Hips,
	Crossed,
	Clasped,
	Chin,
	Belly,
	Raised,
	Fists,
	Cheer,
	Behind,
	Open,
	Shrug,
	Wave,
	Point,
	Beckon,
	Clap,
	Facepalm,
	Piano,
	Stir,
	Water,
	Hold,
	Offer,
	Eat,
	DancePump,
	DanceOpen,
	DanceDisco,
};

// A mood's posture, in degrees: forward lean, lean to the right, the head
// tipped down and to the right, the shoulders raised and drawn forward, and
// what the arms do. Laughter shakes, and some moods sway.
struct FMoodPose
{
	float SpinePitch;
	float SpineRoll;
	float HeadPitch;
	float HeadRoll;
	float ShoulderRaise;
	float ShoulderForward;
	EArms Arms;
	float Shake;
	float Sway;
};

const FMoodPose MoodPoses[] = {
	{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, EArms::None, 0.0f, 0.0f},        // neutral
	{-3.0f, 0.0f, -6.0f, 5.0f, 0.0f, -3.0f, EArms::None, 0.0f, 1.2f},     // happy
	{-6.0f, 0.0f, -12.0f, 3.0f, 2.0f, 0.0f, EArms::Belly, 1.0f, 0.0f},    // laughing
	{-3.0f, 0.0f, -7.0f, 0.0f, 4.0f, 0.0f, EArms::Cheer, 0.0f, 1.6f},     // excited
	{-7.0f, 0.0f, -10.0f, 0.0f, -3.0f, -7.0f, EArms::Hips, 0.0f, 0.0f},   // proud
	{-8.0f, 0.0f, -7.0f, 0.0f, 8.0f, 0.0f, EArms::Raised, 0.0f, 0.0f},    // surprised
	{2.0f, 0.0f, 7.0f, 10.0f, 0.0f, 2.0f, EArms::Chin, 0.0f, 0.0f},       // thinking
	{5.0f, 0.0f, 9.0f, -3.0f, 8.0f, 8.0f, EArms::Clasped, 0.0f, 0.6f},    // worried
	{9.0f, 0.0f, 22.0f, -4.0f, -4.0f, 10.0f, EArms::None, 0.0f, 0.0f},    // sad
	{6.0f, 0.0f, 8.0f, 0.0f, 6.0f, 4.0f, EArms::Fists, 0.0f, 0.0f},       // angry
	{3.0f, 2.0f, 9.0f, 10.0f, -2.0f, 3.0f, EArms::Behind, 0.0f, 1.0f},    // bored
	{-3.0f, -2.0f, 4.0f, -8.0f, 1.0f, 0.0f, EArms::Crossed, 0.0f, 0.0f},  // doubtful
};
static_assert(UE_ARRAY_COUNT(MoodPoses) == static_cast<int32>(ERoomMood::Count));

// Where a hand goes, where its elbow points, which way its fingers and palm
// face, and how far its fingers (and its index finger) curl, from 0 to 1.
struct FArmTarget
{
	FVector Hand = FVector::ZeroVector;
	FVector Pole = FVector::ZeroVector;
	FVector Fingers = FVector::ZeroVector;
	FVector Palm = FVector::ZeroVector;
	float Curl = 0.0f;
	float IndexCurl = 0.0f;
};

// Arm targets added together, by how much of each there is.
struct FArmBlend
{
	float Weight = 0.0f;
	FArmTarget Sum;
	float IndexWeight = 0.0f;

	void Add(const FArmTarget& Target, float W)
	{
		if (W <= 0.0f)
		{
			return;
		}
		Weight += W;
		Sum.Hand += Target.Hand * W;
		Sum.Pole += Target.Pole * W;
		Sum.Fingers += Target.Fingers * W;
		Sum.Palm += Target.Palm * W;
		Sum.Curl += Target.Curl * W;
		Sum.IndexCurl += Target.IndexCurl * W;
	}

	FArmTarget Get() const
	{
		FArmTarget Out;
		if (Weight <= 0.0f)
		{
			return Out;
		}
		Out.Hand = Sum.Hand / Weight;
		Out.Pole = Sum.Pole / Weight;
		Out.Fingers = Sum.Fingers.GetSafeNormal();
		Out.Palm = Sum.Palm.GetSafeNormal();
		Out.Curl = Sum.Curl / Weight;
		Out.IndexCurl = Sum.IndexCurl / Weight;
		return Out;
	}
};

// Where the body's landmarks are, and which way it faces, once its posture
// is set, for its arms' targets.
struct FBody
{
	FVector Pelvis;
	FVector Chest;
	FVector Head;
	FVector Shoulder[2];
	FVector Forward;
	FVector Upward;
	// Its left.
	FVector Across;
	float Time;
	float Seed;
	float Voice;
	FVector GestureDirection;
	float GestureTime;
	double Beat;
};

// The pose of an arm (0 left, 1 right) in a pose of the arms.
FArmTarget ArmPose(EArms Arms, int32 Side, const FBody& B)
{
	const float Sign = Side == 0 ? 1.0f : -1.0f;
	const FVector Out = B.Across * Sign;
	const FVector& F = B.Forward;
	const FVector& U = B.Upward;
	auto At = [&](const FVector& From, float Forward, float Upward, float Outward) {
		return From + F * Forward + U * Upward + Out * Outward;
	};
	const FVector& Shoulder = B.Shoulder[Side];
	FArmTarget T;
	switch (Arms)
	{
	case EArms::Hips:
		T.Hand = At(B.Pelvis, -3.0f, 6.0f, 19.0f);
		T.Pole = At(Shoulder, -28.0f, -22.0f, 34.0f);
		T.Fingers = F * 0.55f - U * 0.8f;
		T.Palm = -Out;
		T.Curl = 0.55f;
		break;
	case EArms::Crossed:
		T.Hand = Side == 0 ? At(B.Chest, 21.0f, -25.0f, -12.0f) : At(B.Chest, 25.0f, -21.0f, -13.0f);
		T.Pole = At(Shoulder, 22.0f, -40.0f, 26.0f);
		T.Fingers = -Out;
		T.Palm = -F;
		T.Curl = 0.3f;
		break;
	case EArms::Clasped:
		T.Hand = At(B.Pelvis, 24.0f, 14.0f, 3.0f);
		T.Pole = At(Shoulder, 6.0f, -40.0f, 22.0f);
		T.Fingers = F * 0.35f - U * 0.9f;
		T.Palm = -Out;
		T.Curl = 0.65f;
		break;
	case EArms::Chin:
		if (Side == 1)
		{
			// A fist under the chin.
			T.Hand = At(B.Head, 9.0f, -13.0f, 1.0f);
			T.Pole = At(Shoulder, 22.0f, -42.0f, 10.0f);
			T.Fingers = U * 0.8f + F * 0.1f - Out * 0.4f;
			T.Palm = -F;
			T.Curl = 0.85f;
		}
		else
		{
			// Holding up the other elbow.
			T.Hand = At(B.Chest, 20.0f, -28.0f, -8.0f);
			T.Pole = At(Shoulder, 18.0f, -40.0f, 22.0f);
			T.Fingers = -Out;
			T.Palm = U;
			T.Curl = 0.3f;
		}
		break;
	case EArms::Belly:
		T.Hand = At(B.Pelvis, 19.0f, 26.0f, 9.0f);
		T.Pole = At(Shoulder, -5.0f, -32.0f, 32.0f);
		T.Fingers = -Out * 0.8f - U * 0.3f;
		T.Palm = -F;
		T.Curl = 0.2f;
		break;
	case EArms::Raised:
		T.Hand = At(Shoulder, 22.0f, -2.0f, 12.0f);
		T.Pole = At(Shoulder, 2.0f, -40.0f, 30.0f);
		T.Fingers = U;
		T.Palm = F;
		T.Curl = 0.05f;
		break;
	case EArms::Fists:
		T.Hand = At(Shoulder, 8.0f, -50.0f, 13.0f);
		T.Pole = At(Shoulder, -30.0f, -28.0f, 18.0f);
		T.Fingers = -U * 0.9f + F * 0.2f;
		T.Palm = -Out;
		T.Curl = 1.0f;
		break;
	case EArms::Cheer:
		T.Hand = At(B.Chest, 26.0f, -6.0f + 4.0f * Sine(B.Time, 2.3f, Side * 0.5f), 17.0f);
		T.Pole = At(Shoulder, 5.0f, -40.0f, 28.0f);
		T.Fingers = U * 0.6f - Out * 0.5f;
		T.Palm = -F;
		T.Curl = 0.95f;
		break;
	case EArms::Behind:
		T.Hand = At(B.Pelvis, -20.0f, 8.0f, 4.0f);
		T.Pole = At(Shoulder, -30.0f, -35.0f, 22.0f);
		T.Fingers = -Out * 0.6f - U * 0.6f;
		T.Palm = F;
		T.Curl = 0.5f;
		break;
	case EArms::Open:
	{
		// Beats as they talk: the hands rise and open, in front, with the voice.
		const float N1 = Noise(B.Time * 0.6f, B.Seed + Side * 2.7f);
		const float N2 = Noise(B.Time * 0.9f, B.Seed + 3.1f + Side * 1.3f);
		T.Hand = At(B.Pelvis, 30.0f + 6.0f * N2, 22.0f + 9.0f * N1 + 10.0f * B.Voice, 24.0f + 6.0f * N2);
		T.Pole = At(Shoulder, -8.0f, -40.0f, 30.0f);
		T.Fingers = F * 0.8f + Out * 0.35f + U * 0.1f;
		T.Palm = U * 0.8f - Out * 0.4f;
		T.Curl = 0.25f;
		break;
	}
	case EArms::Shrug:
		T.Hand = At(B.Pelvis, 26.0f, 20.0f, 36.0f);
		T.Pole = At(Shoulder, -6.0f, -38.0f, 26.0f);
		T.Fingers = F * 0.6f + Out * 0.6f;
		T.Palm = U;
		T.Curl = 0.15f;
		break;
	case EArms::Wave:
	{
		const float Swing = Sine(B.GestureTime, 1.8f);
		T.Hand = At(Shoulder, 12.0f, 34.0f, 20.0f + 10.0f * Swing);
		T.Pole = At(Shoulder, 0.0f, -18.0f, 40.0f);
		T.Fingers = U + Out * 0.15f * Swing;
		T.Palm = F;
		T.Curl = 0.05f;
		break;
	}
	case EArms::Point:
	{
		// Toward what they point at, as long as it's in front of them.
		FVector Direction = B.GestureDirection.GetSafeNormal();
		if ((Direction | F) < 0.3f)
		{
			Direction = (Direction + F * (0.3f - (Direction | F)) * 2.0f).GetSafeNormal();
		}
		T.Hand = Shoulder + Direction * 50.0f;
		T.Pole = Shoulder + Direction * 22.0f - U * 22.0f + Out * 12.0f;
		T.Fingers = Direction;
		T.Palm = -U;
		T.Curl = 0.95f;
		T.IndexCurl = 0.0f;
		return T;
	}
	case EArms::Beckon:
		T.Hand = At(Shoulder, 32.0f, -26.0f, 10.0f);
		T.Pole = At(Shoulder, 0.0f, -38.0f, 26.0f);
		T.Fingers = F;
		T.Palm = U;
		T.Curl = 0.45f + 0.45f * Sine(B.GestureTime, 2.0f);
		break;
	case EArms::Clap:
	{
		const float Apart = FMath::Abs(Sine(B.GestureTime, 1.3f));
		T.Hand = At(B.Chest, 30.0f, -14.0f, 2.0f + 11.0f * Apart);
		T.Pole = At(Shoulder, 6.0f, -36.0f, 28.0f);
		T.Fingers = F * 0.6f + U * 0.6f;
		T.Palm = -Out;
		T.Curl = 0.1f;
		break;
	}
	case EArms::Facepalm:
		T.Hand = At(B.Head, 12.0f, 5.0f, 2.0f);
		T.Pole = At(Shoulder, 24.0f, -34.0f, 16.0f);
		T.Fingers = U * 0.7f - Out * 0.5f;
		T.Palm = -F;
		T.Curl = 0.15f;
		break;
	case EArms::Piano:
	{
		// Hands on the keys in front of them, striking twice a beat, one hand
		// after the other, and wandering up and down the keyboard.
		const float Beat = static_cast<float>(B.Beat);
		const float Strike = FMath::Max(0.0f, Sine(Beat, 2.0f, Side * 0.25f));
		const float Along = 8.0f * Sine(Beat, 0.125f, Side * 0.3f);
		T.Hand = At(B.Pelvis, 44.0f, 6.0f + 2.5f * Strike, 11.0f + Along);
		T.Pole = At(Shoulder, -12.0f, -30.0f, 36.0f);
		T.Fingers = F * 0.9f - U * 0.35f;
		T.Palm = -U;
		T.Curl = 0.35f;
		break;
	}
	case EArms::Stir:
		if (Side == 1)
		{
			// Stirring the pot in front of them.
			T.Hand = At(B.Pelvis, 46.0f + 6.0f * FMath::Cos(B.Time * 3.2f), 22.0f, 4.0f + 6.0f * FMath::Sin(B.Time * 3.2f));
			T.Pole = At(Shoulder, -10.0f, -30.0f, 34.0f);
			T.Fingers = -U * 0.8f + F * 0.3f;
			T.Palm = -Out;
			T.Curl = 0.8f;
		}
		else
		{
			// A hand on the counter.
			T.Hand = At(B.Pelvis, 34.0f, 14.0f, 24.0f);
			T.Pole = At(Shoulder, -12.0f, -30.0f, 36.0f);
			T.Fingers = F;
			T.Palm = -U;
			T.Curl = 0.1f;
		}
		break;
	case EArms::Water:
		// Holding the watering can out over the plants.
		T.Hand = At(Shoulder, 34.0f, -30.0f + 3.0f * Sine(B.Time, 0.4f), 8.0f);
		T.Pole = At(Shoulder, -6.0f, -36.0f, 28.0f);
		T.Fingers = F * 0.8f - U * 0.4f;
		T.Palm = -Out;
		T.Curl = 0.7f;
		break;
	case EArms::Hold:
		// Holding something in front of them.
		T.Hand = At(B.Pelvis, 25.0f, 20.0f, 13.0f);
		T.Pole = At(Shoulder, -8.0f, -38.0f, 26.0f);
		T.Fingers = F * 0.75f - Out * 0.3f;
		T.Palm = -Out;
		T.Curl = 0.6f;
		break;
	case EArms::Offer:
	{
		// Holding it out to whoever it's for.
		FVector Direction = B.GestureDirection.GetSafeNormal();
		Direction.Z = FMath::Clamp(Direction.Z, -0.3f, 0.15f);
		Direction = (Direction + F * 0.3f).GetSafeNormal();
		T.Hand = Shoulder + Direction * 46.0f - U * 12.0f;
		T.Pole = Shoulder + Direction * 18.0f - U * 30.0f + Out * 16.0f;
		T.Fingers = Direction;
		T.Palm = U;
		T.Curl = 0.15f;
		break;
	}
	case EArms::Eat:
	{
		// To the mouth, for a bite or two.
		const float Bite = FMath::Abs(Sine(B.GestureTime, 1.2f));
		T.Hand = At(B.Head, 12.0f + 3.0f * Bite, -10.0f, 1.0f);
		T.Pole = At(Shoulder, 16.0f, -36.0f, 18.0f);
		T.Fingers = U * 0.5f - Out * 0.6f;
		T.Palm = -F;
		T.Curl = 0.6f;
		break;
	}
	case EArms::DancePump:
	{
		// Fists pumping, one and then the other, on the beat.
		const float Lift = FMath::Max(0.0f, FMath::Sin(UE_PI * (static_cast<float>(B.Beat) + Side)));
		T.Hand = At(Shoulder, 22.0f, -18.0f + 32.0f * Lift, 15.0f);
		T.Pole = At(Shoulder, 0.0f, -34.0f, 30.0f);
		T.Fingers = U * 0.6f + F * 0.4f;
		T.Palm = -Out;
		T.Curl = 0.9f;
		break;
	}
	case EArms::DanceOpen:
	{
		// Hands up and open, swaying.
		const float Swing = FMath::Sin(UE_PI * static_cast<float>(B.Beat) + Side * UE_PI);
		T.Hand = At(Shoulder, 16.0f, 12.0f + 10.0f * Swing, 32.0f);
		T.Pole = At(Shoulder, -6.0f, -20.0f, 44.0f);
		T.Fingers = U;
		T.Palm = F;
		T.Curl = 0.15f;
		break;
	}
	case EArms::DanceDisco:
		if (Side == 1)
		{
			// Pointing up and out, and down across, every other beat.
			const float Raised = 0.5f + 0.5f * FMath::Cos(UE_PI * static_cast<float>(B.Beat));
			T.Hand = FMath::Lerp(At(B.Pelvis, 26.0f, 10.0f, -6.0f), At(Shoulder, 14.0f, 42.0f, 26.0f), Raised);
			T.Pole = At(Shoulder, 0.0f, -30.0f, 36.0f);
			T.Fingers = FMath::Lerp(-U * 0.6f - Out * 0.4f, U * 0.8f + Out * 0.3f, Raised);
			T.Palm = -F;
			T.Curl = 0.95f;
			T.IndexCurl = 0.0f;
			return T;
		}
		T.Hand = At(B.Pelvis, -3.0f, 6.0f, 19.0f);
		T.Pole = At(Shoulder, -28.0f, -22.0f, 34.0f);
		T.Fingers = F * 0.55f - U * 0.8f;
		T.Palm = -Out;
		T.Curl = 0.55f;
		break;
	case EArms::None:
		break;
	}
	T.IndexCurl = T.Curl;
	return T;
}

// The skeleton's pose in component space as well as its own, which the rig
// edits: setting a bone in component space moves what hangs from it with it.
class FRigPose
{
public:
	explicit FRigPose(FCompactPose& InPose) : Pose(InPose), Bones(InPose.GetBoneContainer())
	{
		const int32 Num = Pose.GetNumBones();
		Local.SetNumUninitialized(Num);
		Component.SetNumUninitialized(Num);
		Parent.SetNumUninitialized(Num);
		Moved.SetNumZeroed(Num);
		for (const FCompactPoseBoneIndex Index : Pose.ForEachBoneIndex())
		{
			const int32 I = Index.GetInt();
			Local[I] = Pose[Index];
			Parent[I] = Pose.GetParentBoneIndex(Index).GetInt();
			Component[I] = Parent[I] >= 0 ? Local[I] * Component[Parent[I]] : Local[I];
		}
	}

	int32 Find(const TCHAR* Name) const
	{
		const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(FName(Name));
		if (MeshIndex == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		return Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex)).GetInt();
	}

	const FTransform& Get(int32 Bone) const { return Component[Bone]; }

	void Set(int32 Bone, const FTransform& Transform)
	{
		if (Bone < 0)
		{
			return;
		}
		Component[Bone] = Transform;
		Component[Bone].NormalizeRotation();
		Local[Bone] = Parent[Bone] >= 0 ? Component[Bone].GetRelativeTransform(Component[Parent[Bone]]) : Component[Bone];
		// What hangs from it moves with it.
		for (bool& M : Moved)
		{
			M = false;
		}
		Moved[Bone] = true;
		for (int32 I = Bone + 1; I < Component.Num(); ++I)
		{
			if (Parent[I] >= 0 && Moved[Parent[I]])
			{
				Component[I] = Local[I] * Component[Parent[I]];
				Moved[I] = true;
			}
		}
	}

	// Turns a bone about itself, by `Delta` in component space.
	void Rotate(int32 Bone, const FQuat& Delta)
	{
		if (Bone < 0)
		{
			return;
		}
		FTransform Transform = Component[Bone];
		Transform.SetRotation(Delta * Transform.GetRotation());
		Set(Bone, Transform);
	}

	void Write() const
	{
		for (const FCompactPoseBoneIndex Index : Pose.ForEachBoneIndex())
		{
			Pose[Index] = Local[Index.GetInt()];
			Pose[Index].NormalizeRotation();
		}
	}

private:
	FCompactPose& Pose;
	const FBoneContainer& Bones;
	TArray<FTransform> Local;
	TArray<FTransform> Component;
	TArray<int32> Parent;
	TArray<bool> Moved;
};

// Poses an arm toward its target, by `Weight`, with two-bone IK, and turns
// its hand, sharing the hand's twist with the forearm's twist bones.
void PoseArm(FRigPose& Rig, int32 Side, int32 Upper, int32 Lower, int32 Hand, const int32 Twists[2],
	const FArmTarget& Target, float Weight)
{
	if (Weight <= 0.001f || Upper < 0 || Lower < 0 || Hand < 0)
	{
		return;
	}
	Weight = FMath::Min(Weight, 1.0f);
	FTransform Root = Rig.Get(Upper);
	FTransform Joint = Rig.Get(Lower);
	FTransform End = Rig.Get(Hand);
	const FQuat Base = End.GetRotation();

	const FVector Effector = FMath::Lerp(End.GetLocation(), Target.Hand, Weight);
	const FVector Pole = FMath::Lerp(Joint.GetLocation(), Target.Pole, Weight);
	AnimationCore::SolveTwoBoneIK(Root, Joint, End, Pole, Effector, false, 1.0, 1.0);

	// The left hand's fingers are along +X and its palm faces -Y; the right's
	// are mirrored.
	FQuat Turned = Base;
	if (!Target.Fingers.IsNearlyZero() && !Target.Palm.IsNearlyZero())
	{
		const FQuat Goal = Side == 0 ? FRotationMatrix::MakeFromXY(Target.Fingers, -Target.Palm).ToQuat()
									 : FRotationMatrix::MakeFromXY(-Target.Fingers, Target.Palm).ToQuat();
		Turned = FQuat::Slerp(Base, Goal, Weight).GetNormalized();
	}

	Rig.Set(Upper, Root);
	Rig.Set(Lower, Joint);
	Rig.Set(Hand, FTransform(Turned, End.GetLocation(), End.GetScale3D()));

	// The hand's twist about the forearm, shared out along it.
	const FQuat Forearm = Rig.Get(Lower).GetRotation();
	FQuat Swing, Twist;
	(Forearm.Inverse() * Turned).ToSwingTwist(FVector::XAxisVector, Swing, Twist);
	const float Shares[2] = {0.25f, 0.5f};
	for (int32 I = 0; I < 2; ++I)
	{
		if (Twists[I] >= 0)
		{
			const FQuat Part = FQuat::Slerp(FQuat::Identity, Twist, Shares[I]);
			Rig.Rotate(Twists[I], Forearm * Part * Forearm.Inverse());
		}
	}
}

// Curls a hand's fingers toward its palm, from 0 (open) to 1 (a fist).
void CurlFingers(FRigPose& Rig, int32 Side, int32 Hand, float Curl, float IndexCurl)
{
	if (Hand < 0 || (Curl < 0.02f && IndexCurl < 0.02f))
	{
		return;
	}
	const FQuat Rotation = Rig.Get(Hand).GetRotation();
	const FVector Fingers = Rotation.RotateVector(Side == 0 ? FVector::XAxisVector : -FVector::XAxisVector);
	const FVector Palm = Rotation.RotateVector(Side == 0 ? -FVector::YAxisVector : FVector::YAxisVector);
	const FVector Axis = (Fingers ^ Palm).GetSafeNormal();
	if (Axis.IsNearlyZero())
	{
		return;
	}
	static const TCHAR* const Names[] = {TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky")};
	static const float Joints[] = {55.0f, 80.0f, 60.0f};
	const TCHAR* Suffix = Side == 0 ? TEXT("l") : TEXT("r");
	for (int32 Finger = 0; Finger < 4; ++Finger)
	{
		const float Amount = Finger == 0 ? IndexCurl : Curl;
		for (int32 J = 0; J < 3; ++J)
		{
			const int32 Bone = Rig.Find(*FString::Printf(TEXT("%s_0%d_%s"), Names[Finger], J + 1, Suffix));
			Rig.Rotate(Bone, Turn(Axis, Amount * Joints[J]));
		}
	}
	// The thumb folds a little over the others.
	for (int32 J = 1; J < 3; ++J)
	{
		const int32 Bone = Rig.Find(*FString::Printf(TEXT("thumb_0%d_%s"), J + 1, Suffix));
		Rig.Rotate(Bone, Turn(Axis, Curl * 30.0f));
	}
}
// Keeps a foot where it was, with two-bone IK on its leg, the knee bending
// forward, e.g. as the hips drop and sway in a dance.
void PlantFoot(FRigPose& Rig, int32 Thigh, int32 Calf, int32 Foot, const FTransform& Planted)
{
	if (Thigh < 0 || Calf < 0 || Foot < 0)
	{
		return;
	}
	FTransform Root = Rig.Get(Thigh);
	FTransform Joint = Rig.Get(Calf);
	FTransform End = Rig.Get(Foot);
	const FVector Pole = Joint.GetLocation() + Ahead * 40.0f;
	AnimationCore::SolveTwoBoneIK(Root, Joint, End, Pole, Planted.GetLocation(), false, 1.0, 1.0);
	Rig.Set(Thigh, Root);
	Rig.Set(Calf, Joint);
	Rig.Set(Foot, FTransform(Planted.GetRotation(), Planted.GetLocation(), End.GetScale3D()));
}
} // namespace

void RoomRig::Apply(FCompactPose& Pose, const FRoomPoseInput& In)
{
	FRigPose Rig(Pose);
	const int32 Pelvis = Rig.Find(TEXT("pelvis"));
	const int32 Spine[5] = {
		Rig.Find(TEXT("spine_01")),
		Rig.Find(TEXT("spine_02")),
		Rig.Find(TEXT("spine_03")),
		Rig.Find(TEXT("spine_04")),
		Rig.Find(TEXT("spine_05")),
	};
	const int32 Neck[2] = {Rig.Find(TEXT("neck_01")), Rig.Find(TEXT("neck_02"))};
	const int32 Head = Rig.Find(TEXT("head"));
	const int32 Clavicle[2] = {Rig.Find(TEXT("clavicle_l")), Rig.Find(TEXT("clavicle_r"))};
	const int32 Upper[2] = {Rig.Find(TEXT("upperarm_l")), Rig.Find(TEXT("upperarm_r"))};
	const int32 Lower[2] = {Rig.Find(TEXT("lowerarm_l")), Rig.Find(TEXT("lowerarm_r"))};
	const int32 Hand[2] = {Rig.Find(TEXT("hand_l")), Rig.Find(TEXT("hand_r"))};
	const int32 Twists[2][2] = {
		{Rig.Find(TEXT("lowerarm_twist_01_l")), Rig.Find(TEXT("lowerarm_twist_02_l"))},
		{Rig.Find(TEXT("lowerarm_twist_01_r")), Rig.Find(TEXT("lowerarm_twist_02_r"))},
	};
	const int32 Thigh[2] = {Rig.Find(TEXT("thigh_l")), Rig.Find(TEXT("thigh_r"))};
	const int32 Calf[2] = {Rig.Find(TEXT("calf_l")), Rig.Find(TEXT("calf_r"))};
	const int32 Foot[2] = {Rig.Find(TEXT("foot_l")), Rig.Find(TEXT("foot_r"))};
	if (Pelvis < 0 || Spine[4] < 0 || Head < 0 || Upper[0] < 0 || Upper[1] < 0)
	{
		return;
	}
	const float T = In.Time;
	const float Seed = In.Seed;

	//
	// How they hold themselves: their moods, blended, breathing, listening,
	// talking, and any gesture, in degrees.
	//
	float SpinePitch = 0.0f, SpineRoll = 0.0f, SpineYaw = 0.0f;
	float HeadPitch = 0.0f, HeadRoll = 0.0f, HeadYaw = 0.0f;
	float Raise = 0.0f, Forward = 0.0f;
	float Shake = 0.0f, Sway = 0.0f;
	TArray<TPair<EArms, float>, TInlineAllocator<4>> MoodArms;
	for (int32 M = 1; M < static_cast<int32>(ERoomMood::Count); ++M)
	{
		const float W = FMath::Clamp(In.Mood[M], 0.0f, 1.0f);
		if (W <= 0.001f)
		{
			continue;
		}
		const FMoodPose& P = MoodPoses[M];
		SpinePitch += W * P.SpinePitch;
		SpineRoll += W * P.SpineRoll;
		HeadPitch += W * P.HeadPitch;
		HeadRoll += W * P.HeadRoll * (Seed > 0.5f ? 1.0f : -1.0f);
		Raise += W * P.ShoulderRaise;
		Forward += W * P.ShoulderForward;
		Shake += W * P.Shake;
		Sway += W * P.Sway;
		if (P.Arms != EArms::None)
		{
			MoodArms.Add({P.Arms, W});
		}
	}

	// Breathing, and shifting their weight now and then.
	const float Breath = Sine(T, 0.23f, Seed);
	SpinePitch += 0.6f * Breath;
	Raise += 0.5f * Breath;
	SpineRoll += (1.0f + Sway) * Sine(T, 0.06f + 0.02f * Seed, Seed * 3.0f);
	HeadRoll += 1.5f * Sway * Sine(T, 0.11f, Seed * 2.0f);

	// Laughter shakes them.
	if (Shake > 0.0f)
	{
		const float Burst = 0.6f + 0.4f * Sine(T, 0.7f);
		SpinePitch += Shake * 2.5f * Sine(T, 4.5f) * Burst;
		HeadPitch -= Shake * 3.5f * Sine(T, 4.5f, 0.1f) * Burst;
		Raise += Shake * 3.0f * FMath::Abs(Sine(T, 4.5f)) * Burst;
	}

	// Their head moves as they talk, with the voice.
	HeadPitch += In.Talk * (-3.0f * In.Voice + 1.8f * Noise(T * 1.7f, Seed));
	HeadYaw += In.Talk * 3.5f * Noise(T * 0.9f, Seed + 2.0f);
	HeadRoll += In.Talk * 2.0f * Noise(T * 0.6f, Seed + 5.0f);

	// Listening, they tilt their head, and nod now and then.
	HeadRoll += In.Listen * 4.0f * (Seed > 0.5f ? 1.0f : -1.0f);
	const float Beat = FMath::Frac(T / 3.7f + Seed);
	if (Beat < 0.14f)
	{
		HeadPitch += In.Listen * 5.0f * FMath::Sin(UE_PI * Beat / 0.14f);
	}

	// A voice reaching them makes them look up.
	HeadPitch -= 5.0f * In.Attention;
	SpinePitch -= 2.0f * In.Attention;

	// A gesture: the head's, or the arms'.
	const float GestureLength = RoomTypes::GestureSeconds(In.Gesture);
	const float G = Envelope(In.GestureTime, GestureLength) * In.GestureStrength;
	const float Gt = In.GestureTime;
	EArms GestureArms[2] = {EArms::None, EArms::None};
	switch (In.Gesture)
	{
	case ERoomGesture::Nod:
		HeadPitch += G * 11.0f * FMath::Max(Sine(Gt, 2.2f), -0.4f);
		break;
	case ERoomGesture::Shake:
		HeadYaw += G * 16.0f * Sine(Gt, 2.4f);
		break;
	case ERoomGesture::Shrug:
		Raise += G * 14.0f;
		HeadRoll += G * 8.0f;
		GestureArms[0] = GestureArms[1] = EArms::Shrug;
		break;
	case ERoomGesture::Wave:
		GestureArms[1] = EArms::Wave;
		HeadRoll -= G * 4.0f;
		break;
	case ERoomGesture::Point:
		GestureArms[1] = EArms::Point;
		break;
	case ERoomGesture::Bow:
		SpinePitch += G * 22.0f;
		HeadPitch += G * 10.0f;
		break;
	case ERoomGesture::Clap:
		GestureArms[0] = GestureArms[1] = EArms::Clap;
		HeadPitch -= G * 4.0f;
		break;
	case ERoomGesture::Facepalm:
		GestureArms[1] = EArms::Facepalm;
		HeadPitch += G * 14.0f;
		break;
	case ERoomGesture::Laugh:
	{
		const float Burst = 0.6f + 0.4f * Sine(Gt, 0.9f);
		SpinePitch += G * (-4.0f + 2.5f * Sine(Gt, 4.5f) * Burst);
		HeadPitch += G * (-9.0f - 3.5f * Sine(Gt, 4.5f, 0.1f) * Burst);
		Raise += G * 3.0f * FMath::Abs(Sine(Gt, 4.5f));
		GestureArms[0] = GestureArms[1] = EArms::Belly;
		break;
	}
	case ERoomGesture::Beckon:
		GestureArms[1] = EArms::Beckon;
		break;
	case ERoomGesture::Offer:
		GestureArms[1] = EArms::Offer;
		SpinePitch += G * 6.0f;
		break;
	case ERoomGesture::Eat:
		GestureArms[1] = EArms::Eat;
		HeadPitch += G * 6.0f;
		break;
	default:
		break;
	}

	// Busy at something: leaning in to the keys, looking down into the pot or
	// at the plants.
	const float Busy = FMath::Clamp(In.ActivityAmount, 0.0f, 1.0f);
	EArms ActivityArms[2] = {EArms::None, EArms::None};
	switch (In.Activity)
	{
	case ERoomActivity::Piano:
		SpinePitch += Busy * (8.0f + 2.0f * Sine(static_cast<float>(In.Beat), 0.25f));
		HeadPitch += Busy * 10.0f;
		SpineRoll += Busy * 2.5f * Sine(static_cast<float>(In.Beat), 0.25f);
		ActivityArms[0] = ActivityArms[1] = EArms::Piano;
		break;
	case ERoomActivity::Stir:
		SpinePitch += Busy * 7.0f;
		HeadPitch += Busy * 16.0f;
		ActivityArms[0] = ActivityArms[1] = EArms::Stir;
		break;
	case ERoomActivity::Water:
		SpinePitch += Busy * 6.0f;
		HeadPitch += Busy * 18.0f;
		ActivityArms[1] = EArms::Water;
		break;
	case ERoomActivity::None:
		break;
	}

	// Dancing: a bounce on every beat, a sway from side to side, the head
	// nodding along, and the arms in one of a few moves, changing every bar.
	const float Dance = FMath::Clamp(In.Dance, 0.0f, 1.0f) * (1.0f - In.Moving);
	const float DanceBeat = static_cast<float>(In.Beat);
	const float Bounce = 0.5f + 0.5f * FMath::Cos(UE_TWO_PI * DanceBeat);
	EArms DanceArms = EArms::None;
	if (Dance > 0.001f)
	{
		SpineRoll += Dance * 5.0f * FMath::Sin(UE_PI * DanceBeat);
		SpinePitch += Dance * 3.0f * Bounce;
		HeadPitch += Dance * 7.0f * Bounce;
		HeadRoll += Dance * 4.0f * FMath::Sin(UE_PI * DanceBeat + 0.6f);
		Raise += Dance * 3.0f * Bounce;
		static const EArms Moves[] = {EArms::DancePump, EArms::DanceOpen, EArms::DancePump, EArms::DanceDisco};
		DanceArms = Moves[(static_cast<int32>(DanceBeat / 4.0f) + static_cast<int32>(Seed * 4.0f)) % 4];

		// The hips drop on the beat and twist, the feet staying put.
		FTransform Planted[2];
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Foot[Side] >= 0)
			{
				Planted[Side] = Rig.Get(Foot[Side]);
			}
		}
		FTransform Hips = Rig.Get(Pelvis);
		Hips.AddToTranslation(-Up * Dance * 5.0f * Bounce);
		Hips.SetRotation(Turn(Up, Dance * 8.0f * FMath::Sin(UE_PI * DanceBeat + 0.5f)) * Hips.GetRotation());
		Rig.Set(Pelvis, Hips);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Foot[Side] >= 0)
			{
				PlantFoot(Rig, Thigh[Side], Calf[Side], Foot[Side], Planted[Side]);
			}
		}
	}

	// Walking, they hold themselves more plainly, and swing their arms.
	const float Plain = 1.0f - 0.6f * In.Moving;
	SpinePitch *= Plain;
	SpineRoll *= Plain;
	HeadRoll *= Plain;
	Raise *= Plain;
	Forward *= Plain;

	//
	// The spine, the shoulders and the head.
	//
	const float SpineShares[5] = {0.12f, 0.16f, 0.2f, 0.24f, 0.28f};
	FQuat Torso = FQuat::Identity;
	for (int32 I = 0; I < 5; ++I)
	{
		const float S = SpineShares[I];
		const FQuat Delta = Turn(Right, SpinePitch * S) * Turn(-Ahead, SpineRoll * S) * Turn(Up, SpineYaw * S);
		Rig.Rotate(Spine[I], Delta);
		Torso = Delta * Torso;
	}
	const FVector TorsoAhead = Torso.RotateVector(Ahead);
	const FVector TorsoUp = Torso.RotateVector(Up);
	const FVector TorsoLeft = Torso.RotateVector(Left);
	Rig.Rotate(Clavicle[0], Turn(-TorsoAhead, Raise) * Turn(TorsoUp, Forward));
	Rig.Rotate(Clavicle[1], Turn(TorsoAhead, Raise) * Turn(-TorsoUp, Forward));

	const int32 HeadBones[3] = {Neck[0], Neck[1], Head};
	const float HeadShares[3] = {0.25f, 0.3f, 0.45f};
	const FVector TorsoRight = -TorsoLeft;
	for (int32 I = 0; I < 3; ++I)
	{
		const float S = HeadShares[I];
		Rig.Rotate(HeadBones[I], Turn(TorsoRight, HeadPitch * S) * Turn(-TorsoAhead, HeadRoll * S) * Turn(TorsoUp, HeadYaw * S));
	}

	// Where they look, with the spine, the neck and the head turning a share
	// each, the head the whole way.
	if (!In.Look.IsIdentity(1.e-4f))
	{
		const int32 LookBones[5] = {Spine[3], Spine[4], Neck[0], Neck[1], Head};
		const float LookShares[5] = {0.08f, 0.18f, 0.45f, 0.7f, 1.0f};
		FQuat Before = FQuat::Identity;
		for (int32 I = 0; I < 5; ++I)
		{
			const FQuat Now = FQuat::Slerp(FQuat::Identity, In.Look, LookShares[I]);
			Rig.Rotate(LookBones[I], Now * Before.Inverse());
			Before = Now;
		}
	}

	//
	// The arms: a gesture's, then their moods', then beats as they talk, if
	// their hands are free.
	//
	FBody Body;
	Body.Pelvis = Rig.Get(Pelvis).GetLocation();
	Body.Chest = Rig.Get(Spine[4]).GetLocation();
	Body.Head = Rig.Get(Head).GetLocation();
	Body.Shoulder[0] = Rig.Get(Upper[0]).GetLocation();
	Body.Shoulder[1] = Rig.Get(Upper[1]).GetLocation();
	Body.Forward = TorsoAhead;
	Body.Upward = TorsoUp;
	Body.Across = TorsoLeft;
	Body.Time = T;
	Body.Seed = Seed;
	Body.Voice = In.Voice;
	Body.GestureDirection = In.GestureDirection;
	Body.GestureTime = Gt;
	Body.Beat = In.Beat;

	const float ArmsFree = 1.0f - In.Moving;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FArmBlend Blend;
		float Taken = 0.0f;
		if (GestureArms[Side] != EArms::None && G > 0.0f)
		{
			Blend.Add(ArmPose(GestureArms[Side], Side, Body), G);
			Taken += G;
		}
		if (ActivityArms[Side] != EArms::None && Busy > 0.0f)
		{
			const float W = Busy * FMath::Max(0.0f, 1.0f - Taken);
			Blend.Add(ArmPose(ActivityArms[Side], Side, Body), W);
			Taken += W;
		}
		if (DanceArms != EArms::None && Dance > 0.0f)
		{
			const float W = Dance * FMath::Max(0.0f, 1.0f - Taken);
			Blend.Add(ArmPose(DanceArms, Side, Body), W);
			Taken += W;
		}
		if (Side == 1 && In.Hold > 0.0f)
		{
			const float W = In.Hold * 0.85f * FMath::Max(0.0f, 1.0f - Taken);
			Blend.Add(ArmPose(EArms::Hold, Side, Body), W);
			Taken += W;
		}
		const float MoodRoom = FMath::Max(0.0f, 1.0f - Taken);
		for (const TPair<EArms, float>& Arms : MoodArms)
		{
			const float W = Arms.Value * MoodRoom;
			Blend.Add(ArmPose(Arms.Key, Side, Body), W);
			Taken += W;
		}
		const float TalkRoom = FMath::Max(0.0f, 1.0f - Taken);
		Blend.Add(ArmPose(EArms::Open, Side, Body), In.Talk * 0.75f * TalkRoom);
		if (Blend.Weight > 0.001f)
		{
			const FArmTarget Target = Blend.Get();
			const float Weight = FMath::Min(Blend.Weight, 1.0f) * ArmsFree;
			PoseArm(Rig, Side, Upper[Side], Lower[Side], Hand[Side], Twists[Side], Target, Weight);
			CurlFingers(Rig, Side, Hand[Side], Target.Curl * Weight, Target.IndexCurl * Weight);
		}
	}

	Rig.Write();
}
