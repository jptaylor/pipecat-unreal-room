//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

// The microphone on Linux, captured with SDL, since Unreal's audio capture has
// no Linux backend.

#include "PipecatMicrophone.h"

#if PLATFORM_LINUX

THIRD_PARTY_INCLUDES_START
#include <SDL3/SDL.h>
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogPipecatMicrophone, Log, All);

namespace
{
class FPipecatMicrophoneSDL : public IPipecatMicrophoneCapture
{
public:
	explicit FPipecatMicrophoneSDL(FOnAudio InOnAudio) : OnAudio(MoveTemp(InOnAudio)) {}

	virtual ~FPipecatMicrophoneSDL() override
	{
		if (!Stream)
		{
			return;
		}
		// Waits for the audio thread, so OnStreamData isn't called after this.
		SDL_DestroyAudioStream(Stream);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}

	bool Open(int32 SampleRate)
	{
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			UE_LOG(LogPipecatMicrophone, Error, TEXT("Unable to start SDL audio: %hs"), SDL_GetError());
			return false;
		}

		SDL_AudioSpec Spec;
		Spec.format = SDL_AUDIO_S16;
		Spec.channels = 1;
		Spec.freq = SampleRate;
		Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &Spec, &FPipecatMicrophoneSDL::OnStreamData, this);
		if (!Stream)
		{
			UE_LOG(LogPipecatMicrophone, Error, TEXT("Unable to open the microphone: %hs"), SDL_GetError());
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			return false;
		}

		SDL_ResumeAudioStreamDevice(Stream);
		UE_LOG(LogPipecatMicrophone, Log, TEXT("Microphone started at %d Hz"), SampleRate);
		return true;
	}

private:
	static void OnStreamData(void* UserData, SDL_AudioStream* Stream, int AdditionalAmount, int TotalAmount)
	{
		FPipecatMicrophoneSDL* Self = static_cast<FPipecatMicrophoneSDL*>(UserData);

		int16 Buffer[4096];
		int Available = SDL_GetAudioStreamAvailable(Stream);
		while (Available > 0)
		{
			int Read = SDL_GetAudioStreamData(Stream, Buffer, FMath::Min<int>(Available, sizeof(Buffer)));
			if (Read <= 0)
			{
				break;
			}
			Self->OnAudio(Buffer, Read / static_cast<int>(sizeof(int16)));
			Available -= Read;
		}
	}

	FOnAudio OnAudio;
	SDL_AudioStream* Stream = nullptr;
};
} // namespace

TUniquePtr<IPipecatMicrophoneCapture> IPipecatMicrophoneCapture::Start(int32 SampleRate, FOnAudio OnAudio)
{
	TUniquePtr<FPipecatMicrophoneSDL> Capture = MakeUnique<FPipecatMicrophoneSDL>(MoveTemp(OnAudio));
	if (!Capture->Open(SampleRate))
	{
		return nullptr;
	}
	return Capture;
}

#endif // PLATFORM_LINUX
