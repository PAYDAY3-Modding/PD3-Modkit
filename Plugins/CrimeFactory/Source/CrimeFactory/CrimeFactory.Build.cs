// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CrimeFactory : ModuleRules
{
	public CrimeFactory(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new[] {
			"Core",
			"CoreUObject",
			"Engine",
			// Required by the editor interfaces declared in Public headers.
			"Projects",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ExternalImagePicker",
			"PropertyEditor",
			"PluginBrowser",
			"ContentBrowser",
			"ContentBrowserData",
			"DeveloperSettings",
		});
		
		PrivateDependencyModuleNames.AddRange(new[] {
			"Json",
			"InputCore",
			"LevelEditor",
			"EditorStyle",
			"EditorFramework",
			"RenderCore",
			"ImageCore",
			"RHI",
			"UATHelper",
			"DesktopPlatform",
			"AssetRegistry",
            "CollectionManager",
			"AssetTools",
			"EditorScriptingUtilities",
			"ApplicationCore",
			"PluginUtils",
			"ToolMenus",
			"Settings"
		});
	}
}
