//
// Copyright (c) 2026, Daily
//
// SPDX-License-Identifier: BSD-2-Clause
//

using UnrealBuildTool;

public class PipecatRoom : ModuleRules
{
	public PipecatRoom(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"AnimationCore",
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"MediaAssets",
			"Pipecat",
			"ProceduralMeshComponent",
			"Slate",
			"SlateCore",
		});

		// For PipecatDualSense, which reads a PlayStation controller.
		PrivateDependencyModuleNames.AddRange(new string[] {
			"ApplicationCore",
			"InputCore",
			"InputDevice",
		});
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "hid.lib", "setupapi.lib" });
		}

		// For DLSS and its Frame Generation, where NVIDIA's DLSS plugins have
		// been added to Plugins (see the README).
		bool bDLSS = HasPlugin(Target, "DLSS");
		if (bDLSS)
		{
			PrivateDependencyModuleNames.Add("DLSSBlueprint");
		}
		PublicDefinitions.Add("WITH_DLSS=" + (bDLSS ? "1" : "0"));
		bool bFrameGeneration = HasPlugin(Target, "StreamlineDLSSG");
		if (bFrameGeneration)
		{
			PrivateDependencyModuleNames.Add("StreamlineDLSSGBlueprint");
		}
		PublicDefinitions.Add("WITH_DLSS_FRAME_GENERATION=" + (bFrameGeneration ? "1" : "0"));
	}

	// Whether one of NVIDIA's plugins is in the project's Plugins, on Windows.
	static bool HasPlugin(ReadOnlyTargetRules Target, string Name)
	{
		return Target.Platform == UnrealTargetPlatform.Win64
			&& Target.ProjectFile != null
			&& System.IO.Directory.Exists(System.IO.Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins", Name));
	}
}
