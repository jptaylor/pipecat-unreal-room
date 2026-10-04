//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "PipecatMicrophone.h"

#include "Misc/ScopeLock.h"

#include <cmath>

namespace
{
// How many of the last samples it keeps, e.g. 128 ms at 16 kHz.
const int32 RecentSamples = 2048;
} // namespace

FPipecatMicrophone::~FPipecatMicrophone()
{
	Stop();
}

bool FPipecatMicrophone::Start(int32 SampleRate, FOnAudio InOnAudio)
{
	if (Capture)
	{
		return true;
	}

	OnAudio = MoveTemp(InOnAudio);
	FramesPerChunk = SampleRate / 100;
	Pending.Reset();
	{
		FScopeLock Lock(&RecentLock);
		Recent.Init(0, RecentSamples);
		Next = 0;
	}

	Capture = IPipecatMicrophoneCapture::Start(
		SampleRate, [this](const int16* Frames, int32 NumFrames) { HandleAudio(Frames, NumFrames); });
	return Capture.IsValid();
}

void FPipecatMicrophone::Stop()
{
	// Waits for the audio thread, so HandleAudio isn't called after this.
	Capture.Reset();
	Level = 0.0f;
	Rms = 0.0f;
}

void FPipecatMicrophone::GetWaveform(TArray<float>& OutSamples, int32 NumSamples) const
{
	FScopeLock Lock(&RecentLock);
	NumSamples = FMath::Clamp(NumSamples, 0, Recent.Num());
	OutSamples.SetNumUninitialized(NumSamples);
	for (int32 i = 0; i < NumSamples; ++i)
	{
		const int32 Index = (Next - NumSamples + i + Recent.Num()) % FMath::Max(Recent.Num(), 1);
		OutSamples[i] = Recent[Index] / 32768.0f;
	}
}

void FPipecatMicrophone::HandleAudio(const int16* Frames, int32 NumFrames)
{
	double Sum = 0.0;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		Sum += static_cast<double>(Frames[i]) * Frames[i];
	}
	const float FrameRms = NumFrames > 0 ? static_cast<float>(std::sqrt(Sum / NumFrames) / 32768.0) : 0.0f;
	Rms = FrameRms;
	Level = FMath::Clamp(FrameRms * 4.0f, 0.0f, 1.0f);

	{
		FScopeLock Lock(&RecentLock);
		if (Recent.Num() > 0)
		{
			for (int32 i = 0; i < NumFrames; ++i)
			{
				Recent[Next] = Frames[i];
				Next = (Next + 1) % Recent.Num();
			}
		}
	}

	// The transport sends 10 ms at a time.
	Pending.Append(Frames, NumFrames);
	int32 Sent = 0;
	while (Pending.Num() - Sent >= FramesPerChunk)
	{
		OnAudio(Pending.GetData() + Sent, FramesPerChunk);
		Sent += FramesPerChunk;
	}
	if (Sent > 0)
	{
		Pending.RemoveAt(0, Sent, EAllowShrinking::No);
	}
}
