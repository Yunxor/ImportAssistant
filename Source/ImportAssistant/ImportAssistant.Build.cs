using UnrealBuildTool;

public class ImportAssistant : ModuleRules
{
    public ImportAssistant(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Slate", "SlateCore", "ToolMenus", "AssetRegistry",
            "ContentBrowser", "DesktopPlatform", "ApplicationCore", "InputCore", "InterchangeCore", "InterchangeEngine", "PropertyEditor", "Projects", "AssetDefinition", "DirectoryWatcher"
        });
    }
}





