using UnrealBuildTool;
public class Suzie : ModuleRules
{
	public Suzie(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		OptimizeCode = CodeOptimization.Never;

		PublicIncludePaths.AddRange(new string[] {}); 
		
		// UBT filters missing include directories before the prebuild generator runs.
		// Keep this directory available even immediately after a clean build.
		string NativeBodiesDirectory = System.IO.Path.Combine(PluginDirectory, "Intermediate", "NativeBodies");
		System.IO.Directory.CreateDirectory(NativeBodiesDirectory);
		PrivateIncludePaths.Add(NativeBodiesDirectory);
		ExternalDependencies.Add(System.IO.Path.Combine(PluginDirectory, "Tools", "GenerateNativeBodies.ps1"));


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"GameplayTags",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Blutility",
				"Json",
				"UnrealEd",
				"Projects",
				"BlueprintGraph",
				"UMG",
				"SlateCore",
				"zlib",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(new string[] {});
	}
}
