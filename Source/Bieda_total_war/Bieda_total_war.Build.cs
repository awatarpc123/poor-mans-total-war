// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Bieda_total_war : ModuleRules
{
	public Bieda_total_war(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"MassEntity", "MassCommon", "StructUtils"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// Slate UI (for battle HUD)
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
