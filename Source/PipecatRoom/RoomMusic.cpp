//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "RoomMusic.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"

namespace
{
// The synthesizer's sample rate, and how far ahead of what's heard it makes
// its music, in seconds.
constexpr int32 Rate = 32000;
constexpr float Ahead = 0.2f;

// How loud the house's music and sounds are, e.g. 0 to try it silently.
TAutoConsoleVariable<float> CVarMusicVolume(
	TEXT("room.MusicVolume"), 1.0f, TEXT("How loud the house's music and sounds are, from 0 (silent, still playing) to 1"));

float Midi(float Note)
{
	return 440.0f * FMath::Pow(2.0f, (Note - 69.0f) / 12.0f);
}

// A chord: its root, as a MIDI note, and whether it's minor.
struct FChord
{
	int32 Root;
	bool bMinor;
};

// Round they go: C, G, A minor, F for the piano, and A minor, F, C, G to
// dance to.
const FChord PianoChords[] = {{48, false}, {43, false}, {45, true}, {41, false}};
const FChord LaterPianoChords[] = {{45, true}, {41, false}, {48, false}, {43, false}};
const FChord DanceChords[] = {{45, true}, {41, false}, {48, false}, {43, false}};
} // namespace

// Makes the music, a voice at a time: each note is a voice, with its own
// sound, that fades away. Patterns of notes play on a grid of sixteenths,
// following the chords round.
class FRoomSynth
{
public:
	enum class EKind : uint8
	{
		Keys,
		Bass,
		Kick,
		Hat,
		Clap,
		Pluck,
		Bell,
		Pop,
		Plink,
	};

	struct FVoice
	{
		EKind Kind = EKind::Keys;
		float Age = 0.0f;
		float Length = 1.0f;
		float Freq = 440.0f;
		float Amp = 0.0f;
		float Decay = 0.5f;
		float Delay = 0.0f;
		float Phase[5] = {};
		float Last = 0.0f;
	};

	void Start(ERoomMusicStyle InStyle)
	{
		Style = InStyle;
		bPlaying = true;
		Bpm = Style == ERoomMusicStyle::Dance ? 118.0 : 84.0;
		Clock = 0;
		Step = 0;
		NextStep = 0.0;
	}

	void Stop() { bPlaying = false; }
	bool IsSounding() const { return bPlaying || Voices.Num() > 0; }
	int64 GetClock() const { return Clock; }
	double GetBpm() const { return Bpm; }

	void Add(EKind Kind, float Freq, float Amp, float Decay, float Delay = 0.0f)
	{
		FVoice Voice;
		Voice.Kind = Kind;
		Voice.Freq = Freq;
		Voice.Amp = Amp;
		Voice.Decay = Decay;
		Voice.Delay = Delay;
		switch (Kind)
		{
		case EKind::Kick:
			Voice.Length = 0.4f;
			break;
		case EKind::Hat:
			Voice.Length = 0.12f;
			break;
		case EKind::Clap:
			Voice.Length = 0.3f;
			break;
		case EKind::Pop:
			Voice.Length = 0.25f;
			break;
		case EKind::Plink:
			Voice.Length = 0.8f;
			break;
		case EKind::Bell:
			Voice.Length = Decay * 3.0f;
			break;
		default:
			Voice.Length = Decay * 5.0f;
			break;
		}
		if (Voices.Num() < 48)
		{
			Voices.Add(Voice);
		}
	}

	// A few notes up and down a pentatonic scale, as someone tries the piano.
	void Notes()
	{
		static const int32 Scale[] = {60, 62, 64, 67, 69, 72, 74, 76, 79};
		int32 At = Random.RandRange(1, 5);
		const int32 Count = Random.RandRange(5, 8);
		for (int32 I = 0; I < Count; ++I)
		{
			At = FMath::Clamp(At + Random.RandRange(-2, 2), 0, UE_ARRAY_COUNT(Scale) - 1);
			const float When = I * Random.FRandRange(0.18f, 0.3f);
			Add(EKind::Keys, Midi(Scale[At]), 0.28f, 0.9f, When);
			if (I % 3 == 0)
			{
				Add(EKind::Keys, Midi(Scale[At] - 12), 0.16f, 1.1f, When);
			}
		}
	}

	void Render(int16* Out, int32 Frames, float Gain, float& OutLoudness)
	{
		const float Dt = 1.0f / Rate;
		const double StepFrames = Rate * 60.0 / Bpm / 4.0;
		float Peak = 0.0f;
		for (int32 F = 0; F < Frames; ++F)
		{
			while (bPlaying && Clock >= NextStep)
			{
				Trigger(Step);
				++Step;
				NextStep += StepFrames;
			}
			float Sum = 0.0f;
			for (FVoice& Voice : Voices)
			{
				if (Voice.Delay > 0.0f)
				{
					Voice.Delay -= Dt;
					continue;
				}
				Sum += Sample(Voice, Dt);
				Voice.Age += Dt;
			}
			const float Mixed = FMath::Tanh(Sum * Gain * 1.3f) * 0.85f;
			Peak = FMath::Max(Peak, FMath::Abs(Mixed));
			Out[F] = static_cast<int16>(FMath::Clamp(Mixed, -1.0f, 1.0f) * 32000.0f);
			++Clock;
		}
		Voices.RemoveAll([](const FVoice& Voice) { return Voice.Delay <= 0.0f && Voice.Age > Voice.Length; });
		OutLoudness = Peak;
	}

private:
	float Osc(FVoice& Voice, int32 Index, float Freq, float Dt)
	{
		Voice.Phase[Index] += Freq * Dt;
		Voice.Phase[Index] -= FMath::FloorToFloat(Voice.Phase[Index]);
		return FMath::Sin(UE_TWO_PI * Voice.Phase[Index]);
	}

	float Noise()
	{
		NoiseState ^= NoiseState << 13;
		NoiseState ^= NoiseState >> 17;
		NoiseState ^= NoiseState << 5;
		return static_cast<float>(NoiseState) / 2147483648.0f - 1.0f;
	}

	float Sample(FVoice& V, float Dt)
	{
		const float Age = V.Age;
		const float F = V.Freq;
		switch (V.Kind)
		{
		case EKind::Keys:
		{
			// A soft electric piano: a few harmonics, struck, ringing away.
			const float Env = FMath::Min(Age / 0.004f, 1.0f) * FMath::Exp(-Age / V.Decay);
			const float S = Osc(V, 0, F, Dt) + 0.35f * Osc(V, 1, 2.0f * F, Dt) + 0.12f * Osc(V, 2, 3.0f * F, Dt)
				+ 0.05f * Osc(V, 3, 4.01f * F, Dt) * FMath::Exp(-Age / 0.15f);
			return S * Env * V.Amp;
		}
		case EKind::Bass:
		{
			const float Env = FMath::Min(Age / 0.005f, 1.0f) * FMath::Exp(-Age / V.Decay);
			const float S = Osc(V, 0, F, Dt) + 0.5f * Osc(V, 1, 2.0f * F, Dt) + 0.22f * Osc(V, 2, 3.0f * F, Dt);
			return S * Env * V.Amp;
		}
		case EKind::Kick:
		{
			const float Pitch = 48.0f + 110.0f * FMath::Exp(-Age / 0.035f);
			return Osc(V, 0, Pitch, Dt) * FMath::Exp(-Age / 0.16f) * V.Amp;
		}
		case EKind::Hat:
		{
			const float N = Noise();
			const float High = N - V.Last;
			V.Last = N;
			return High * 0.5f * FMath::Exp(-Age / 0.025f) * V.Amp;
		}
		case EKind::Clap:
		{
			const float N = Noise();
			const float Band = 0.5f * (N + V.Last);
			V.Last = N;
			const float Bursts = Age < 0.03f ? (FMath::Frac(Age / 0.01f) < 0.5f ? 1.0f : 0.4f) : 1.0f;
			return (Band * Bursts * FMath::Exp(-Age / 0.07f) + 0.3f * Osc(V, 0, 190.0f, Dt) * FMath::Exp(-Age / 0.05f)) * V.Amp;
		}
		case EKind::Pluck:
		{
			const float Env = FMath::Min(Age / 0.002f, 1.0f) * FMath::Exp(-Age / V.Decay);
			return (Osc(V, 0, F, Dt) + 0.3f * Osc(V, 1, 3.0f * F, Dt)) * Env * V.Amp;
		}
		case EKind::Bell:
		{
			// A bell's partials, out of tune with each other, each dying away at its own rate.
			static const float Ratios[] = {1.0f, 2.0f, 2.76f, 4.07f, 5.43f};
			static const float Amps[] = {1.0f, 0.55f, 0.45f, 0.3f, 0.18f};
			static const float Decays[] = {1.0f, 0.65f, 0.46f, 0.31f, 0.19f};
			float S = 0.0f;
			for (int32 I = 0; I < 5; ++I)
			{
				S += Amps[I] * Osc(V, I, Ratios[I] * F, Dt) * FMath::Exp(-Age / (V.Decay * Decays[I]));
			}
			return S * FMath::Min(Age / 0.002f, 1.0f) * V.Amp * 0.5f;
		}
		case EKind::Pop:
		{
			const float Pitch = 300.0f + 900.0f * FMath::Min(Age / 0.08f, 1.0f);
			return Osc(V, 0, Pitch, Dt) * FMath::Exp(-Age / 0.05f) * V.Amp;
		}
		case EKind::Plink:
		{
			const float Ring = (Osc(V, 0, F, Dt) + 0.5f * Osc(V, 1, 2.7f * F, Dt)) * FMath::Exp(-Age / 0.12f);
			const float Plop = Osc(V, 2, 60.0f + 140.0f * FMath::Exp(-Age / 0.05f), Dt) * FMath::Exp(-Age / 0.06f);
			return (Ring + 0.6f * Plop) * V.Amp;
		}
		}
		return 0.0f;
	}

	// What plays on one sixteenth of the grid.
	void Trigger(int32 At)
	{
		const int32 S = At % 16;
		const int32 Bar = At / 16;
		const bool bThird = Random.FRand() < 0.5f;
		if (Style == ERoomMusicStyle::Piano)
		{
			const FChord& C = ((Bar / 8) % 2 ? LaterPianoChords : PianoChords)[Bar % 4];
			const int32 Third = C.bMinor ? 3 : 4;
			if (S == 0)
			{
				Add(EKind::Keys, Midi(C.Root), 0.3f, 1.6f);
			}
			if (S == 8)
			{
				Add(EKind::Keys, Midi(C.Root + 7), 0.2f, 1.2f);
			}
			if (S % 2 == 0)
			{
				static const int32 Pattern[] = {0, 1, 2, 3, 2, 1, 2, 3};
				const int32 Tones[] = {C.Root + 12, C.Root + 12 + Third, C.Root + 19, C.Root + 24};
				Add(EKind::Keys, Midi(Tones[Pattern[(S / 2) % 8]]), 0.12f + 0.03f * Random.FRand(), 0.8f);
			}
			if (S == 0 || S == 8 || (S == 12 && bThird) || (S == 6 && Random.FRand() < 0.3f))
			{
				const int32 Melody[] = {0, Third, 7, 12, 14, 9};
				const int32 Note = C.Root + 24 + Melody[Random.RandRange(0, S == 6 ? 5 : 3)];
				Add(EKind::Keys, Midi(Note), 0.2f, 1.3f);
			}
			return;
		}

		const FChord& C = DanceChords[Bar % 4];
		const int32 Third = C.bMinor ? 3 : 4;
		if (S % 4 == 0)
		{
			Add(EKind::Kick, 0.0f, 0.9f, 0.2f);
		}
		if (S % 4 == 2)
		{
			Add(EKind::Hat, 0.0f, 0.35f, 0.05f);
		}
		else if (S % 2 == 1)
		{
			Add(EKind::Hat, 0.0f, 0.1f, 0.05f);
		}
		if (S == 4 || S == 12)
		{
			Add(EKind::Clap, 0.0f, 0.45f, 0.1f);
		}
		if (S % 2 == 0)
		{
			Add(EKind::Bass, Midi(S % 4 == 2 ? C.Root : C.Root - 12), 0.32f, 0.16f);
		}
		if (S == 6 || S == 14)
		{
			for (const int32 Tone : {C.Root + 12, C.Root + 12 + Third, C.Root + 19})
			{
				Add(EKind::Keys, Midi(Tone), 0.08f, 0.14f);
			}
		}
		if (Bar % 2 == 1 && S % 2 == 0)
		{
			const int32 Arp[] = {0, Third, 7, 12};
			Add(EKind::Pluck, Midi(C.Root + 24 + Arp[(S / 2) % 4]), 0.06f, 0.12f);
		}
	}

	ERoomMusicStyle Style = ERoomMusicStyle::Piano;
	bool bPlaying = false;
	double Bpm = 84.0;
	int64 Clock = 0;
	int32 Step = 0;
	double NextStep = 0.0;
	TArray<FVoice> Voices;
	FRandomStream Random {static_cast<int32>(FPlatformTime::Cycles())};
	uint32 NoiseState = 0x9E3779B9u;

	friend class URoomMusic;
};

URoomMusic::URoomMusic()
{
	PrimaryComponentTick.bCanEverTick = true;
}

URoomMusic::~URoomMusic()
{
	delete Synth;
}

void URoomMusic::BeginPlay()
{
	Super::BeginPlay();
	Ensure();
}

void URoomMusic::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Audio)
	{
		Audio->Stop();
	}
	Super::EndPlay(EndPlayReason);
}

void URoomMusic::Ensure()
{
	if (Audio)
	{
		return;
	}
	Synth = new FRoomSynth();
	Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(Rate);
	Wave->NumChannels = 1;
	Wave->Duration = INDEFINITELY_LOOPING_DURATION;
	Wave->bLooping = false;
	Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;

	Audio = NewObject<UAudioComponent>(GetOwner(), NAME_None);
	Audio->SetupAttachment(this);
	Audio->bAutoActivate = false;
	Audio->bAllowSpatialization = true;
	Audio->bOverrideAttenuation = true;
	SetReach(Inner, Falloff);
	Audio->SetSound(Wave);
	Audio->RegisterComponent();
	Audio->Play();
}

void URoomMusic::SetReach(float InInner, float InFalloff)
{
	Inner = InInner;
	Falloff = InFalloff;
	if (!Audio)
	{
		return;
	}
	FSoundAttenuationSettings& A = Audio->AttenuationOverrides;
	A.bAttenuate = true;
	A.bSpatialize = true;
	A.AttenuationShapeExtents = FVector(Inner);
	A.FalloffDistance = Falloff;
	A.bEnableOcclusion = true;
	A.OcclusionTraceChannel = ECC_Visibility;
	A.OcclusionLowPassFilterFrequency = 1500.0f;
	A.OcclusionVolumeAttenuation = 0.4f;
	A.OcclusionInterpolationTime = 0.4f;
}

int32 URoomMusic::Queued() const
{
	return Wave ? Wave->GetAvailableAudioByteCount() / static_cast<int32>(sizeof(int16)) : 0;
}

void URoomMusic::Play(ERoomMusicStyle InStyle)
{
	Ensure();
	Style = InStyle;
	bPlaying = true;
	Synth->Start(InStyle);
}

void URoomMusic::Stop()
{
	bPlaying = false;
	if (Synth)
	{
		Synth->Stop();
	}
}

void URoomMusic::PlayNotes()
{
	Ensure();
	Synth->Notes();
}

double URoomMusic::GetBeat() const
{
	if (!Synth || !bPlaying)
	{
		return 0.0;
	}
	const double Heard = FMath::Max<double>(0.0, static_cast<double>(Synth->GetClock() - Queued()));
	return Heard / Rate * Synth->GetBpm() / 60.0;
}

void URoomMusic::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	Ensure();

	// Quieter while someone speaks over it, easing.
	Volume = FMath::FInterpTo(Volume, 1.0f - 0.55f * Duck, DeltaTime, 4.0f);
	Audio->SetVolumeMultiplier(FMath::Max(Volume * CVarMusicVolume.GetValueOnGameThread(), 0.0001f));

	if (!Synth->IsSounding())
	{
		Loudness = FMath::FInterpTo(Loudness, 0.0f, DeltaTime, 6.0f);
		return;
	}
	// Made a little ahead of what's heard, a block at a time.
	int32 Need = static_cast<int32>(Rate * Ahead) - Queued();
	if (Need <= 0)
	{
		return;
	}
	Need = Align(Need, 256);
	Buffer.SetNumUninitialized(Need, EAllowShrinking::No);
	float Peak = 0.0f;
	Synth->Render(Buffer.GetData(), Need, 0.55f, Peak);
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Buffer.GetData()), Need * sizeof(int16));
	Loudness = FMath::Max(Peak, FMath::FInterpTo(Loudness, Peak, DeltaTime, 8.0f));
}

void URoomMusic::PlaySound(UWorld* World, const FVector& Where, ERoomSound Sound, float SoundVolume)
{
	if (!World || !World->GetWorldSettings())
	{
		return;
	}
	// Made all at once, and played from where it happens.
	FRoomSynth Synth;
	float Seconds = 0.5f;
	float Reach = 1600.0f;
	switch (Sound)
	{
	case ERoomSound::Bell:
		for (int32 Ring = 0; Ring < 3; ++Ring)
		{
			Synth.Add(FRoomSynth::EKind::Bell, 880.0f, 0.9f, 1.6f, 0.45f * Ring);
		}
		Seconds = 5.0f;
		Reach = 6000.0f;
		break;
	case ERoomSound::Pop:
		Synth.Add(FRoomSynth::EKind::Pop, 0.0f, 0.4f, 0.1f);
		Seconds = 0.3f;
		break;
	case ERoomSound::Plink:
		Synth.Add(FRoomSynth::EKind::Plink, 1800.0f, 0.5f, 0.2f);
		Seconds = 0.9f;
		break;
	case ERoomSound::Chime:
		for (int32 I = 0; I < 3; ++I)
		{
			Synth.Add(FRoomSynth::EKind::Keys, Midi(72 + 4 * I), 0.25f, 0.6f, 0.09f * I);
		}
		Seconds = 1.6f;
		break;
	}
	const int32 Frames = static_cast<int32>(Rate * Seconds);
	TArray<int16> Samples;
	Samples.SetNumUninitialized(Frames);
	float Peak = 0.0f;
	Synth.Render(Samples.GetData(), Frames, 0.8f, Peak);

	USoundWaveProcedural* SoundWave = NewObject<USoundWaveProcedural>(World->GetWorldSettings());
	SoundWave->SetSampleRate(Rate);
	SoundWave->NumChannels = 1;
	SoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
	SoundWave->bLooping = false;
	SoundWave->QueueAudio(reinterpret_cast<const uint8*>(Samples.GetData()), Frames * sizeof(int16));

	UAudioComponent* Player = NewObject<UAudioComponent>(World->GetWorldSettings());
	Player->bAutoActivate = false;
	Player->bAllowSpatialization = true;
	Player->bOverrideAttenuation = true;
	FSoundAttenuationSettings& A = Player->AttenuationOverrides;
	A.bAttenuate = true;
	A.bSpatialize = true;
	A.AttenuationShapeExtents = FVector(300.0f);
	A.FalloffDistance = Reach;
	A.bEnableOcclusion = true;
	A.OcclusionVolumeAttenuation = 0.5f;
	Player->SetSound(SoundWave);
	Player->SetVolumeMultiplier(FMath::Max(SoundVolume * CVarMusicVolume.GetValueOnGameThread(), 0.0001f));
	Player->SetWorldLocation(Where);
	Player->RegisterComponentWithWorld(World);
	Player->Play();
	FTimerHandle Done;
	World->GetTimerManager().SetTimer(Done, FTimerDelegate::CreateWeakLambda(Player, [Player]() {
		Player->Stop();
		Player->DestroyComponent();
	}), Seconds + 0.2f, false);
}
