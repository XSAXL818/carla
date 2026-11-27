#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TestPlatformSettings.generated.h"

/**
 * Project Settings > Game > Test Platform.
 *
 * The Slate config panel is the front end of an entry level, not an overlay on
 * every map: it is shown when a session starts in a map listed here, so
 * browsing the towns in the editor stays clean. The list lives in
 * [/Script/TestPlatform.TestPlatformSettings] of DefaultGame.ini, and the
 * module falls back to DefaultEntryMap when the list is empty.
 *
 * Read once at module startup: editing the list needs a restart, like any other
 * project setting that the runtime caches.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Test Platform"))
class TESTPLATFORM_API UTestPlatformSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTestPlatformSettings();

	/** The entry level shipped in Content/TestPlatform/Maps. */
	static const TCHAR* const DefaultEntryMap;

	/**
	 * Maps that host the config panel. An entry matches either the full package
	 * path (/Game/TestPlatform/Maps/L_TestPlatformEntry) or the short map name
	 * (L_TestPlatformEntry); the PIE prefix is ignored. Every other map runs
	 * with no panel. -TestPlatformForceUI overrides the gate at launch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Panel", meta = (DisplayName = "Entry maps"))
	TArray<FString> EntryMaps;
};
