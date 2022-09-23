// Copyright shenkns Planet Gravity System Developed With Unreal Engine. All Rights Reserved 2022.

using UnrealBuildTool;

public class PlanetGravitySystem : ModuleRules
{
	public PlanetGravitySystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PrivateIncludePaths.AddRange(
			new string[] 
			{
				"PlanetGravitySystem/Public/"
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"PerfCounters",
				"PhysicsCore"
			}
		);
	}
}
