#include "BlueprintExporterSettings.h"

UBlueprintExporterSettings::UBlueprintExporterSettings()
{
	ConfigOnlyParentClassWhitelist = {
		FSoftClassPath(TEXT("/Script/GameplayAbilities.GameplayAbility")),
		FSoftClassPath(TEXT("/Script/GameplayAbilities.GameplayEffect")),
		FSoftClassPath(TEXT("/Script/GameplayAbilities.GameplayCueNotify_Actor")),
		FSoftClassPath(TEXT("/Script/GameplayAbilities.GameplayCueNotify_Static")),
	};
}
