using UnrealBuildTool;

public class CrimeFactoryEditorVisualizers : ModuleRules
{
    public CrimeFactoryEditorVisualizers(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
        PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "Settings" });
    }
}
