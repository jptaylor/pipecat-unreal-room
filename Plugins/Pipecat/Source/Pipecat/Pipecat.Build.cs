//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

using System.IO;
using UnrealBuildTool;

public class Pipecat : ModuleRules
{
	public Pipecat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The Pipecat C++ client reports errors with exceptions.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
		});

		// The Pipecat C++ client and its transports, with the libraries they
		// need, built in ThirdParty/<platform>, e.g. ThirdParty/Linux, by
		// ThirdParty/build-linux.sh with Unreal's toolchain (the WebSocket
		// transport), or by ThirdParty/build-windows.ps1 with Visual Studio's
		// (the Daily and WebSocket transports).
		string ThirdParty = Path.Combine(PluginDirectory, "ThirdParty", Target.Platform.ToString());
		PublicSystemIncludePaths.Add(Path.Combine(ThirdParty, "include"));
		string Libraries = Target.Platform == UnrealTargetPlatform.Win64 ? "*.lib" : "*.a";
		foreach (string Library in Directory.GetFiles(Path.Combine(ThirdParty, "lib"), Libraries))
		{
			PublicAdditionalLibraries.Add(Library);
		}

		// libcurl (with the libraries it needs) starts bots, and OpenSSL secures
		// the WebSocket.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "libcurl", "nghttp2", "OpenSSL", "zlib");

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// What the WebSocket transport's libraries need from Windows.
			PublicSystemLibraries.AddRange(new string[] { "ws2_32.lib", "iphlpapi.lib", "bcrypt.lib" });

			// The Daily transport, with Daily's Core SDK, a DLL next to the
			// plugin's.
			string DailyCore = Path.Combine(ThirdParty, "bin", "daily_core.dll");
			if (File.Exists(DailyCore))
			{
				PublicDefinitions.Add("PIPECAT_WITH_DAILY=1");
				RuntimeDependencies.Add("$(BinaryOutputDir)/daily_core.dll", DailyCore);
			}
			else
			{
				PublicDefinitions.Add("PIPECAT_WITH_DAILY=0");
			}
		}
		else
		{
			PublicDefinitions.Add("PIPECAT_WITH_DAILY=0");
		}

		// The microphone: SDL captures it on Linux (PipecatMicrophoneSDL.cpp),
		// since Unreal's audio capture has no Linux backend, and Unreal's audio
		// capture elsewhere (PipecatMicrophoneAudioCapture.cpp), with its
		// resampler converting it to the sample rate the bot is sent.
		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "SDL3");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[] {
				"AudioCapture",
				"AudioCaptureCore",
				"AudioPlatformConfiguration",
			});
		}
	}
}
