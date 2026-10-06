using UnrealBuildTool;

public class ReferenceWorldTarget : TargetRules
{
	public ReferenceWorldTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ReferenceWorld");
	}
}
