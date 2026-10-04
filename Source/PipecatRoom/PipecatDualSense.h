//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "HAL/Runnable.h"
#include "IInputDevice.h"

#include <atomic>

class FRunnableThread;

// A PlayStation controller, a DualSense or a DualShock 4, over USB or
// Bluetooth. On Windows, Unreal only reads Xbox controllers itself, so this
// reads the controller's HID reports and sends its sticks, triggers and
// buttons on as the standard gamepad keys, e.g. Gamepad_Left2D.
class FPipecatDualSense : public IInputDevice, public FRunnable
{
public:
	explicit FPipecatDualSense(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler);
	virtual ~FPipecatDualSense() override;

	/** Stops reading the controller. */
	void Shutdown();

	// IInputDevice
	virtual void Tick(float DeltaTime) override {}
	virtual void SendControllerEvents() override;
	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }
	virtual void SetChannelValue(int32 ControllerId, FForceFeedbackChannelType ChannelType, float Value) override {}
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override {}
	virtual bool SupportsForceFeedback(int32 ControllerId) override { return false; }
	virtual bool IsGamepadAttached() const override;

	// FRunnable, which reads the controller off the game thread.
	virtual uint32 Run() override;
	virtual void Stop() override;

	enum EAxis
	{
		LeftX,
		LeftY,
		RightX,
		RightY,
		LeftTrigger,
		RightTrigger,
		NumAxes,
	};

	// The controller as of its latest report: axes from -1 to 1 (up and
	// right positive), triggers from 0 to 1, and a bit per button, in the
	// order of the Buttons array in the .cpp.
	struct FPadState
	{
		float Axes[NumAxes] = {};
		uint32 Buttons = 0;
	};

private:
	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;

	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopping{false};

	// Written by the reading thread, read by the game thread.
	mutable FCriticalSection Lock;
	FPadState Latest;
	bool bConnected = false;

	// The game thread's: what it last sent on, and as whose.
	FPadState Sent;
	bool bSentConnected = false;
	FInputDeviceId DeviceId = INPUTDEVICEID_NONE;
	FPlatformUserId UserId = PLATFORMUSERID_NONE;
	double NextRepeatTimes[32] = {};
	float InitialButtonRepeatDelay = 0.2f;
	float ButtonRepeatDelay = 0.1f;
};
