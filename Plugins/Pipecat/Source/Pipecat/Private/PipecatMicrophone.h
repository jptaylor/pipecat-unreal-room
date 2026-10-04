//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

#include <atomic>

// Captures the default microphone as 16-bit mono PCM, from when it starts until
// it's destroyed. Each platform has its own: SDL's on Linux, where Unreal's
// audio capture has no backend, and Unreal's audio capture elsewhere.
class IPipecatMicrophoneCapture
{
public:
	// Called on the capture's audio thread.
	using FOnAudio = TFunction<void(const int16* Frames, int32 NumFrames)>;

	// Starts capturing at the given sample rate, or returns null if it can't:
	// from the microphone whose name has `Device` in it, or by default the
	// system's, unless that's a game controller's.
	static TUniquePtr<IPipecatMicrophoneCapture> Start(int32 SampleRate, FOnAudio OnAudio, const FString& Device);

	// Stops capturing, once OnAudio has returned.
	virtual ~IPipecatMicrophoneCapture() = default;
};

// Captures the default microphone as 16-bit mono PCM, and passes it on in
// 10 ms chunks. It keeps the last few hundredths of a second of it, e.g. to
// draw its waveform.
class FPipecatMicrophone
{
public:
	// Called on the audio thread.
	using FOnAudio = IPipecatMicrophoneCapture::FOnAudio;

	~FPipecatMicrophone();

	bool Start(int32 SampleRate, FOnAudio InOnAudio, const FString& Device = FString());
	void Stop();

	// How loud the microphone is, from 0 to 1.
	float GetLevel() const { return Level.load(); }

	// How loud the microphone is, as the RMS of its last 10 ms, from 0 to 1
	// (full scale), unscaled.
	float GetRms() const { return Rms.load(); }

	// The last `NumSamples` samples it captured, oldest first, from -1 to 1.
	void GetWaveform(TArray<float>& OutSamples, int32 NumSamples) const;

private:
	void HandleAudio(const int16* Frames, int32 NumFrames);

	TUniquePtr<IPipecatMicrophoneCapture> Capture;
	FOnAudio OnAudio;
	int32 FramesPerChunk = 0;
	TArray<int16> Pending;
	std::atomic<float> Level {0.0f};
	std::atomic<float> Rms {0.0f};

	// The last samples captured, in a ring, where the next one goes at Next.
	mutable FCriticalSection RecentLock;
	TArray<int16> Recent;
	int32 Next = 0;
};
