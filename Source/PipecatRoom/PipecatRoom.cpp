//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "Features/IModularFeatures.h"
#include "IInputDeviceModule.h"
#include "Modules/ModuleManager.h"
#include "PipecatDualSense.h"

// The game's module. It also gives the game a PlayStation controller to read,
// which Unreal doesn't on Windows.
class FPipecatRoomModule : public IInputDeviceModule
{
public:
	virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override
	{
#if PLATFORM_WINDOWS
		TSharedPtr<FPipecatDualSense> Device = MakeShared<FPipecatDualSense>(InMessageHandler);
		DualSense = Device;
		return Device;
#else
		return nullptr;
#endif
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), this);
#if PLATFORM_WINDOWS
		// The application keeps the device after the module's gone, so its
		// thread, whose code is the module's, stops now.
		if (TSharedPtr<FPipecatDualSense> Device = DualSense.Pin())
		{
			Device->Shutdown();
		}
#endif
	}

	virtual bool IsGameModule() const override
	{
		return true;
	}

private:
#if PLATFORM_WINDOWS
	TWeakPtr<FPipecatDualSense> DualSense;
#endif
};

IMPLEMENT_PRIMARY_GAME_MODULE(FPipecatRoomModule, PipecatRoom, "PipecatRoom");
