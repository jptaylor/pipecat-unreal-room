//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

#include "PipecatDualSense.h"

#if PLATFORM_WINDOWS

#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ConfigCacheIni.h"

#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogPipecatDualSense, Log, All);

namespace PipecatDualSense
{
	enum class EPad
	{
		None,
		DualSense,
		DualShock4,
	};

	EPad GetPad(uint16 VendorId, uint16 ProductId)
	{
		if (VendorId != 0x054C) // Sony
		{
			return EPad::None;
		}
		switch (ProductId)
		{
		case 0x0CE6: // DualSense
		case 0x0DF2: // DualSense Edge
			return EPad::DualSense;
		case 0x05C4: // DualShock 4
		case 0x09CC: // DualShock 4, its second version
		case 0x0BA0: // DualShock 4, through Sony's USB wireless adapter
			return EPad::DualShock4;
		default:
			return EPad::None;
		}
	}

	enum EButton
	{
		Cross,
		Circle,
		Square,
		Triangle,
		L1,
		R1,
		L2,
		R2,
		Create,
		Options,
		L3,
		R3,
		Up,
		Down,
		Left,
		Right,
		LeftStickUp,
		LeftStickDown,
		LeftStickLeft,
		LeftStickRight,
		RightStickUp,
		RightStickDown,
		RightStickLeft,
		RightStickRight,
		NumButtons,
	};

	// The Unreal key of each of the buttons above, laid out as on an Xbox
	// controller, e.g. Cross is A.
	const FGamepadKeyNames::Type& GetButtonKey(int32 Button)
	{
		static const FGamepadKeyNames::Type Keys[] = {
			FGamepadKeyNames::FaceButtonBottom,
			FGamepadKeyNames::FaceButtonRight,
			FGamepadKeyNames::FaceButtonLeft,
			FGamepadKeyNames::FaceButtonTop,
			FGamepadKeyNames::LeftShoulder,
			FGamepadKeyNames::RightShoulder,
			FGamepadKeyNames::LeftTriggerThreshold,
			FGamepadKeyNames::RightTriggerThreshold,
			FGamepadKeyNames::SpecialLeft,
			FGamepadKeyNames::SpecialRight,
			FGamepadKeyNames::LeftThumb,
			FGamepadKeyNames::RightThumb,
			FGamepadKeyNames::DPadUp,
			FGamepadKeyNames::DPadDown,
			FGamepadKeyNames::DPadLeft,
			FGamepadKeyNames::DPadRight,
			FGamepadKeyNames::LeftStickUp,
			FGamepadKeyNames::LeftStickDown,
			FGamepadKeyNames::LeftStickLeft,
			FGamepadKeyNames::LeftStickRight,
			FGamepadKeyNames::RightStickUp,
			FGamepadKeyNames::RightStickDown,
			FGamepadKeyNames::RightStickLeft,
			FGamepadKeyNames::RightStickRight,
		};
		static_assert(UE_ARRAY_COUNT(Keys) == NumButtons);
		return Keys[Button];
	}

	const FGamepadKeyNames::Type& GetAxisKey(int32 Axis)
	{
		static const FGamepadKeyNames::Type Keys[] = {
			FGamepadKeyNames::LeftAnalogX,
			FGamepadKeyNames::LeftAnalogY,
			FGamepadKeyNames::RightAnalogX,
			FGamepadKeyNames::RightAnalogY,
			FGamepadKeyNames::LeftTriggerAnalog,
			FGamepadKeyNames::RightTriggerAnalog,
		};
		static_assert(UE_ARRAY_COUNT(Keys) == FPipecatDualSense::NumAxes);
		return Keys[Axis];
	}

	// How far a stick is pushed for it to count as pressing, e.g.,
	// Gamepad_LeftStick_Up, like the sticks' dead zone.
	constexpr float StickButtonThreshold = 0.25f;

	// Reads a report from the controller. The reports come in two layouts. A
	// DualSense's full one, over USB, or over Bluetooth once something (e.g.
	// Steam) has asked for it: the sticks, the triggers, a counter, then the
	// buttons. And a simple one, a DualSense's over Bluetooth until then, and
	// a DualShock 4's: the sticks, the buttons, then the triggers.
	bool ParseReport(EPad Pad, bool bBluetooth, const uint8* Report, uint32 Length, FPipecatDualSense::FPadState& Out)
	{
		if (Length == 0)
		{
			return false;
		}

		uint32 Sticks = 0;
		uint32 Buttons = 0;
		uint32 Triggers = 0;
		if (Pad == EPad::DualSense && Report[0] == 0x01 && !bBluetooth)
		{
			Sticks = 1;
			Triggers = 5;
			Buttons = 8;
		}
		else if (Pad == EPad::DualSense && Report[0] == 0x31)
		{
			Sticks = 2;
			Triggers = 6;
			Buttons = 9;
		}
		else if (Report[0] == 0x01)
		{
			Sticks = 1;
			Buttons = 5;
			Triggers = 8;
		}
		else if (Pad == EPad::DualShock4 && Report[0] == 0x11)
		{
			Sticks = 3;
			Buttons = 7;
			Triggers = 10;
		}
		else
		{
			return false;
		}
		if (Length < FMath::Max(Buttons + 3, Triggers + 2))
		{
			return false;
		}

		using EAxis = FPipecatDualSense::EAxis;
		auto Stick = [](uint8 Value) { return FMath::Clamp((Value - 128) / 127.0f, -1.0f, 1.0f); };
		Out.Axes[EAxis::LeftX] = Stick(Report[Sticks]);
		Out.Axes[EAxis::LeftY] = -Stick(Report[Sticks + 1]);
		Out.Axes[EAxis::RightX] = Stick(Report[Sticks + 2]);
		Out.Axes[EAxis::RightY] = -Stick(Report[Sticks + 3]);
		Out.Axes[EAxis::LeftTrigger] = Report[Triggers] / 255.0f;
		Out.Axes[EAxis::RightTrigger] = Report[Triggers + 1] / 255.0f;

		const uint8 Face = Report[Buttons];
		const uint8 Shoulders = Report[Buttons + 1];
		const uint8 Other = Report[Buttons + 2];
		// The d-pad's direction, in eighths clockwise from up, or 8 if it's
		// not pressed.
		const uint8 DPad = Face & 0x0F;

		uint32 Bits = 0;
		auto Set = [&Bits](EButton Button, bool bDown)
		{
			if (bDown)
			{
				Bits |= 1u << Button;
			}
		};
		Set(Square, Face & 0x10);
		Set(Cross, Face & 0x20);
		Set(Circle, Face & 0x40);
		Set(Triangle, Face & 0x80);
		Set(Up, DPad == 7 || DPad == 0 || DPad == 1);
		Set(Right, DPad >= 1 && DPad <= 3);
		Set(Down, DPad >= 3 && DPad <= 5);
		Set(Left, DPad >= 5 && DPad <= 7);
		Set(L1, Shoulders & 0x01);
		Set(R1, Shoulders & 0x02);
		Set(L2, Shoulders & 0x04);
		Set(R2, Shoulders & 0x08);
		// The touchpad's click is Gamepad_Special_Left too, as on a PlayStation.
		Set(Create, (Shoulders & 0x10) || (Other & 0x02));
		Set(Options, Shoulders & 0x20);
		Set(L3, Shoulders & 0x40);
		Set(R3, Shoulders & 0x80);
		Set(LeftStickUp, Out.Axes[EAxis::LeftY] > StickButtonThreshold);
		Set(LeftStickDown, Out.Axes[EAxis::LeftY] < -StickButtonThreshold);
		Set(LeftStickLeft, Out.Axes[EAxis::LeftX] < -StickButtonThreshold);
		Set(LeftStickRight, Out.Axes[EAxis::LeftX] > StickButtonThreshold);
		Set(RightStickUp, Out.Axes[EAxis::RightY] > StickButtonThreshold);
		Set(RightStickDown, Out.Axes[EAxis::RightY] < -StickButtonThreshold);
		Set(RightStickLeft, Out.Axes[EAxis::RightX] < -StickButtonThreshold);
		Set(RightStickRight, Out.Axes[EAxis::RightX] > StickButtonThreshold);
		Out.Buttons = Bits;
		return true;
	}

	// Opens the first PlayStation controller connected, for reading, or
	// returns INVALID_HANDLE_VALUE if there isn't one.
	HANDLE OpenPad(EPad& OutPad, bool& bOutBluetooth, uint32& OutReportLength)
	{
		GUID HidGuid;
		HidD_GetHidGuid(&HidGuid);
		const HDEVINFO Devices = SetupDiGetClassDevsW(&HidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (Devices == INVALID_HANDLE_VALUE)
		{
			return INVALID_HANDLE_VALUE;
		}

		HANDLE Result = INVALID_HANDLE_VALUE;
		SP_DEVICE_INTERFACE_DATA Interface = {};
		Interface.cbSize = sizeof(Interface);
		for (DWORD Index = 0; Result == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(Devices, nullptr, &HidGuid, Index, &Interface); ++Index)
		{
			DWORD Size = 0;
			SetupDiGetDeviceInterfaceDetailW(Devices, &Interface, nullptr, 0, &Size, nullptr);
			if (Size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
			{
				continue;
			}
			TArray<uint8> DetailData;
			DetailData.SetNumZeroed(Size);
			SP_DEVICE_INTERFACE_DETAIL_DATA_W* Detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(DetailData.GetData());
			Detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			if (!SetupDiGetDeviceInterfaceDetailW(Devices, &Interface, Detail, Size, nullptr, nullptr))
			{
				continue;
			}

			// Opened without access first, which is enough to see what it
			// is, so other devices, e.g. keyboards, are left alone.
			HANDLE Device = CreateFileW(Detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (Device == INVALID_HANDLE_VALUE)
			{
				continue;
			}
			HIDD_ATTRIBUTES Attributes = {};
			Attributes.Size = sizeof(Attributes);
			EPad Pad = HidD_GetAttributes(Device, &Attributes) ? GetPad(Attributes.VendorID, Attributes.ProductID) : EPad::None;
			HIDP_CAPS Caps = {};
			PHIDP_PREPARSED_DATA Preparsed = nullptr;
			if (Pad != EPad::None && HidD_GetPreparsedData(Device, &Preparsed))
			{
				// Only its gamepad, not e.g. its audio's controls.
				const bool bGamepad = HidP_GetCaps(Preparsed, &Caps) == HIDP_STATUS_SUCCESS && Caps.UsagePage == 0x01 && Caps.Usage == 0x05;
				HidD_FreePreparsedData(Preparsed);
				if (!bGamepad)
				{
					Pad = EPad::None;
				}
			}
			CloseHandle(Device);
			if (Pad == EPad::None)
			{
				continue;
			}

			Result = CreateFileW(Detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			if (Result != INVALID_HANDLE_VALUE)
			{
				OutPad = Pad;
				OutReportLength = Caps.InputReportByteLength;
				// The Bluetooth HID service's ID.
				bOutBluetooth = FString(Detail->DevicePath).Contains(TEXT("00001124-0000-1000-8000-00805f9b34fb"));
			}
		}
		SetupDiDestroyDeviceInfoList(Devices);
		return Result;
	}
}

FPipecatDualSense::FPipecatDualSense(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("InitialButtonRepeatDelay"), InitialButtonRepeatDelay, GInputIni);
	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("ButtonRepeatDelay"), ButtonRepeatDelay, GInputIni);

	Thread = FRunnableThread::Create(this, TEXT("PipecatDualSense"), 0, TPri_AboveNormal);
}

FPipecatDualSense::~FPipecatDualSense()
{
	Shutdown();
}

void FPipecatDualSense::Shutdown()
{
	if (Thread)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
}

void FPipecatDualSense::Stop()
{
	bStopping = true;
}

uint32 FPipecatDualSense::Run()
{
	using namespace PipecatDualSense;

	while (!bStopping)
	{
		EPad Pad = EPad::None;
		bool bBluetooth = false;
		uint32 ReportLength = 0;
		const HANDLE Device = OpenPad(Pad, bBluetooth, ReportLength);
		if (Device == INVALID_HANDLE_VALUE)
		{
			// Looks again in a couple of seconds, in case one's connected.
			for (int32 Wait = 0; Wait < 20 && !bStopping; ++Wait)
			{
				FPlatformProcess::Sleep(0.1f);
			}
			continue;
		}
		UE_LOG(LogPipecatDualSense, Log, TEXT("Reading a %s over %s."),
			Pad == EPad::DualSense ? TEXT("DualSense") : TEXT("DualShock 4"), bBluetooth ? TEXT("Bluetooth") : TEXT("USB"));

		TArray<uint8> Report;
		Report.SetNumZeroed(FMath::Max<uint32>(ReportLength, 64));
		OVERLAPPED Overlapped = {};
		Overlapped.hEvent = CreateEventW(nullptr, true, false, nullptr);
		while (!bStopping)
		{
			ResetEvent(Overlapped.hEvent);
			if (!ReadFile(Device, Report.GetData(), Report.Num(), nullptr, &Overlapped) && GetLastError() != ERROR_IO_PENDING)
			{
				break;
			}
			// Waits for its next report, checking now and then whether to stop.
			DWORD Wait = WAIT_TIMEOUT;
			while (!bStopping && (Wait = WaitForSingleObject(Overlapped.hEvent, 100)) == WAIT_TIMEOUT)
			{
			}
			DWORD Read = 0;
			if (Wait != WAIT_OBJECT_0)
			{
				CancelIoEx(Device, &Overlapped);
				GetOverlappedResult(Device, &Overlapped, &Read, true);
				break;
			}
			// It fails when the controller's turned off or unplugged.
			if (!GetOverlappedResult(Device, &Overlapped, &Read, false))
			{
				break;
			}

			FPadState State;
			if (ParseReport(Pad, bBluetooth, Report.GetData(), Read, State))
			{
				FScopeLock ScopeLock(&Lock);
				Latest = State;
				bConnected = true;
			}
		}
		CloseHandle(Overlapped.hEvent);
		CloseHandle(Device);

		{
			FScopeLock ScopeLock(&Lock);
			Latest = FPadState();
			bConnected = false;
		}
		UE_LOG(LogPipecatDualSense, Log, TEXT("Stopped reading the controller."));
	}
	return 0;
}

bool FPipecatDualSense::IsGamepadAttached() const
{
	FScopeLock ScopeLock(&Lock);
	return bConnected;
}

void FPipecatDualSense::SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	MessageHandler = InMessageHandler;
}

void FPipecatDualSense::SendControllerEvents()
{
	using namespace PipecatDualSense;

	FPadState State;
	bool bNowConnected = false;
	{
		FScopeLock ScopeLock(&Lock);
		State = Latest;
		bNowConnected = bConnected;
	}

	// When it connects, it's paired with a player, as an Xbox controller is:
	// the first, unless another controller already is.
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	if (bNowConnected && !bSentConnected)
	{
		if (!DeviceId.IsValid())
		{
			DeviceId = DeviceMapper.AllocateNewInputDeviceId();
		}
		UserId = DeviceMapper.GetPlatformUserForNewlyConnectedDevice();
		DeviceMapper.Internal_MapInputDeviceToUser(DeviceId, UserId, EInputDeviceConnectionState::Connected);
	}
	if (!DeviceId.IsValid())
	{
		return;
	}

	// When it's disconnected, its state is all zeros, which releases
	// everything that was held.
	for (int32 Axis = 0; Axis < NumAxes; ++Axis)
	{
		if (State.Axes[Axis] != Sent.Axes[Axis])
		{
			MessageHandler->OnControllerAnalog(GetAxisKey(Axis), UserId, DeviceId, State.Axes[Axis]);
		}
	}

	const double Now = FPlatformTime::Seconds();
	for (int32 Button = 0; Button < NumButtons; ++Button)
	{
		const bool bDown = (State.Buttons & (1u << Button)) != 0;
		const bool bWasDown = (Sent.Buttons & (1u << Button)) != 0;
		if (bDown && !bWasDown)
		{
			MessageHandler->OnControllerButtonPressed(GetButtonKey(Button), UserId, DeviceId, false);
			NextRepeatTimes[Button] = Now + InitialButtonRepeatDelay;
		}
		else if (!bDown && bWasDown)
		{
			MessageHandler->OnControllerButtonReleased(GetButtonKey(Button), UserId, DeviceId, false);
		}
		else if (bDown && NextRepeatTimes[Button] <= Now)
		{
			MessageHandler->OnControllerButtonPressed(GetButtonKey(Button), UserId, DeviceId, true);
			NextRepeatTimes[Button] = Now + ButtonRepeatDelay;
		}
	}

	if (!bNowConnected && bSentConnected)
	{
		UserId = DeviceMapper.GetUserForUnpairedInputDevices();
		DeviceMapper.Internal_MapInputDeviceToUser(DeviceId, UserId, EInputDeviceConnectionState::Disconnected);
	}
	bSentConnected = bNowConnected;
	Sent = State;
}

#endif // PLATFORM_WINDOWS
