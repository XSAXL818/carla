using UnrealBuildTool;

public class TestPlatform : ModuleRules
{
	public TestPlatform(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Runtime module: ships in packaged builds. The panel lives on the game
		// viewport and drives the camera through the player controller, so no
		// editor-only dependency is allowed here.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"ApplicationCore",
			"HTTP",
			"Json",
			"JsonUtilities",
			"Projects",
			// UTestPlatformSettings: the entry-map whitelist in Project Settings
			// (DefaultGame.ini). Runtime module, safe for packaged builds.
			"DeveloperSettings"
		});

		if (Target.bBuildEditor)
		{
			// Only for a friendly "not in editor" hint; no editor UI is used.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd" });
		}
	}
}
