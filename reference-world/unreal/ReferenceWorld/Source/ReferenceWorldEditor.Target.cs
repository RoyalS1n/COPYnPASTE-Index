using UnrealBuildTool;

public class ReferenceWorldEditorTarget : TargetRules
{
	public ReferenceWorldEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ReferenceWorld");
	}
}
