#include "TestPlatformSettings.h"

const TCHAR* const UTestPlatformSettings::DefaultEntryMap =
	TEXT("/Game/TestPlatform/Maps/L_TestPlatformEntry");

UTestPlatformSettings::UTestPlatformSettings()
{
	// Deliberately empty: DefaultGame.ini ships the list, and the module falls
	// back to DefaultEntryMap when the list ends up empty, so an operator who
	// clears the list still gets the config screen instead of no panel at all.
	// Keeping the default out of the constructor also keeps the Project
	// Settings page honest: it shows exactly what the ini file says.
	EntryMaps.Reset();
}
