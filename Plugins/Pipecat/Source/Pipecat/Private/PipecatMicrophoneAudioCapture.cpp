//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

// The microphone everywhere but Linux, captured with Unreal's audio capture,
// which has no Linux backend.

#include "PipecatMicrophone.h"

#if !PLATFORM_LINUX

#include "AudioCaptureCore.h"
#include "AudioResampler.h"

DEFINE_LOG_CATEGORY_STATIC(LogPipecatMicrophone, Log, All);

namespace
{
class FPipecatMicrophoneAudioCapture : public IPipecatMicrophoneCapture
{
public:
	explicit FPipecatMicrophoneAudioCapture(FOnAudio InOnAudio) : OnAudio(MoveTemp(InOnAudio)) {}

	virtual ~FPipecatMicrophoneAudioCapture() override
	{
		if (!Capture.IsStreamOpen())
		{
			return;
		}
		// Waits for the audio thread, so OnCapture isn't called after this.
		Capture.AbortStream();
	}

	bool Open(int32 InSampleRate)
	{
		SampleRate = InSampleRate;

		// The default microphone, as it captures: 32-bit float, with its own
		// channels and sample rate.
		Audio::FAudioCaptureDeviceParams Params;
		Audio::FOnAudioCaptureFunction OnCaptured = [this](
			const void* Samples, int32 NumFrames, int32 NumChannels, int32 CaptureSampleRate, double StreamTime, bool bOverflow) {
			OnCapture(static_cast<const float*>(Samples), NumFrames, NumChannels, CaptureSampleRate);
		};
		if (!Capture.OpenAudioCaptureStream(Params, MoveTemp(OnCaptured), 1024) || !Capture.StartStream())
		{
			UE_LOG(LogPipecatMicrophone, Error, TEXT("Unable to open the microphone"));
			Capture.AbortStream();
			return false;
		}

		UE_LOG(LogPipecatMicrophone, Log, TEXT("Microphone started at %d Hz"), SampleRate);
		return true;
	}

private:
	void OnCapture(const float* Samples, int32 NumFrames, int32 NumChannels, int32 CaptureSampleRate)
	{
		if (NumFrames <= 0 || NumChannels <= 0 || CaptureSampleRate <= 0)
		{
			return;
		}

		// To mono.
		Mono.SetNumUninitialized(NumFrames, EAllowShrinking::No);
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			float Sum = 0.0f;
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				Sum += Samples[Frame * NumChannels + Channel];
			}
			Mono[Frame] = Sum / NumChannels;
		}

		// To the sample rate asked for.
		const float* Output = Mono.GetData();
		int32 NumOutput = NumFrames;
		if (CaptureSampleRate != SampleRate)
		{
			const float Ratio = static_cast<float>(SampleRate) / CaptureSampleRate;
			if (ResampledSampleRate != CaptureSampleRate)
			{
				Resampler.Init(Audio::EResamplingMethod::FastSinc, Ratio, 1);
				ResampledSampleRate = CaptureSampleRate;
			}
			Resampled.SetNumUninitialized(FMath::CeilToInt(NumFrames * Ratio) + 16, EAllowShrinking::No);
			NumOutput = 0;
			Resampler.ProcessAudio(Mono.GetData(), NumFrames, false, Resampled.GetData(), Resampled.Num(), NumOutput);
			Output = Resampled.GetData();
		}

		// To 16-bit.
		Converted.SetNumUninitialized(NumOutput, EAllowShrinking::No);
		for (int32 i = 0; i < NumOutput; ++i)
		{
			Converted[i] = static_cast<int16>(FMath::Clamp(Output[i], -1.0f, 1.0f) * 32767.0f);
		}
		OnAudio(Converted.GetData(), NumOutput);
	}

	FOnAudio OnAudio;
	Audio::FAudioCapture Capture;
	Audio::FResampler Resampler;
	int32 SampleRate = 0;
	// The microphone's sample rate the resampler converts from.
	int32 ResampledSampleRate = 0;
	// Only used on the audio thread.
	TArray<float> Mono;
	TArray<float> Resampled;
	TArray<int16> Converted;
};
} // namespace

TUniquePtr<IPipecatMicrophoneCapture> IPipecatMicrophoneCapture::Start(int32 SampleRate, FOnAudio OnAudio)
{
	TUniquePtr<FPipecatMicrophoneAudioCapture> Capture = MakeUnique<FPipecatMicrophoneAudioCapture>(MoveTemp(OnAudio));
	if (!Capture->Open(SampleRate))
	{
		return nullptr;
	}
	return Capture;
}

#endif // !PLATFORM_LINUX
