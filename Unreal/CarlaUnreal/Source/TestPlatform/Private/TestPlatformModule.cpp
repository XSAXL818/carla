#include "TestPlatformModule.h"

#include "STestPlatformUI.h"
#include "TestPlatformApi.h"
#include "TestPlatformSettings.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogTestPlatform, Log, All);

#define LOCTEXT_NAMESPACE "TestPlatformModule"

namespace
{
	FTestPlatformModule* GModuleInstance = nullptr;
	const TCHAR* DefaultPython = TEXT("C:/SoftWare/MiniConda/envs/chat_carla/python.exe");

	/**
	 * Fixed title of the window the backend runs in. The operator finds the
	 * backend by this name and stops it by closing that window, so nothing is
	 * allowed to rename it (see --suppressApplicationTitle below).
	 */
	const TCHAR* BackendWindowTitle = TEXT("CARLA TestPlatform Backend");

	/** cmd.exe, for the console fallback when Windows Terminal cannot be used. */
	FString ConsoleShell()
	{
		const FString ComSpec = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
		return ComSpec.IsEmpty() ? FString(TEXT("C:/Windows/System32/cmd.exe")) : ComSpec;
	}

	/** 0 disables the automatic run.py spawn so a hand-started backend wins. */
	TAutoConsoleVariable<int32> CVarBackendAutoStart(
		TEXT("TestPlatform.BackendAutoStart"),
		1,
		TEXT("Spawn the Python backend automatically when nothing answers on the backend URL (0 disables)."),
		ECVF_Default);

	/**
	 * Split the resolved backend root URL into the host and port that the run.py
	 * spawn needs. The GET /health probe is an HTTP request to the root URL as-is,
	 * so it does not go through this split. False when the URL has no usable host.
	 */
	bool ParseBackendEndpoint(const FString& Url, FString& OutHost, int32& OutPort)
	{
		OutHost.Reset();
		OutPort = 0;

		FString Rest = Url.TrimStartAndEnd();
		bool bHttps = false;
		const int32 SchemeEnd = Rest.Find(TEXT("://"));
		if (SchemeEnd != INDEX_NONE)
		{
			bHttps = Rest.Left(SchemeEnd).Equals(TEXT("https"), ESearchCase::IgnoreCase);
			Rest = Rest.RightChop(SchemeEnd + 3);
		}
		const int32 PathStart = Rest.Find(TEXT("/"));
		if (PathStart != INDEX_NONE)
		{
			Rest.LeftInline(PathStart);
		}
		if (Rest.IsEmpty())
		{
			return false;
		}

		const int32 PortSep = Rest.Find(TEXT(":"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (PortSep == INDEX_NONE)
		{
			OutHost = Rest;
			OutPort = bHttps ? 443 : 80;
		}
		else
		{
			OutHost = Rest.Left(PortSep);
			OutPort = FCString::Atoi(*Rest.Mid(PortSep + 1));
		}
		if (OutHost.IsEmpty() || OutPort <= 0 || OutPort > 65535)
		{
			return false;
		}
		if (OutHost.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			OutHost = TEXT("127.0.0.1");
		}
		return true;
	}

	bool IsLoopbackHost(const FString& Host)
	{
		return Host == TEXT("127.0.0.1") || Host == TEXT("::1") || Host.StartsWith(TEXT("127."));
	}

	/** World instance ids of a PIE world: UEDPIE_0_<map>, UEDPIE_12_<map>, ... */
	FString StripPiePrefix(const FString& MapName)
	{
		FString Name = MapName;
		if (Name.StartsWith(TEXT("UEDPIE_"), ESearchCase::CaseSensitive))
		{
			const int32 PrefixEnd = Name.Find(
				TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 7);
			if (PrefixEnd != INDEX_NONE)
			{
				Name.RightChopInline(PrefixEnd + 1);
			}
		}
		return Name;
	}

	/** The config-driven whitelist, with the shipped entry level as the fallback. */
	TArray<FString> LoadEntryMaps()
	{
		TArray<FString> Maps;
		if (const UTestPlatformSettings* Settings = GetDefault<UTestPlatformSettings>())
		{
			for (const FString& Map : Settings->EntryMaps)
			{
				const FString Trimmed = Map.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					Maps.AddUnique(Trimmed);
				}
			}
		}
		if (Maps.Num() == 0)
		{
			Maps.Add(UTestPlatformSettings::DefaultEntryMap);
		}
		return Maps;
	}
}

void FTestPlatformModule::StartupModule()
{
	UE_LOG(LogTestPlatform, Log, TEXT("CARLA test platform runtime module started"));
	GModuleInstance = this;

	// The panel belongs to the entry level, so the map gate is read here once:
	// this is a launch-time decision, like the backend URL.
	bForceUI = FParse::Param(FCommandLine::Get(), TEXT("TestPlatformForceUI"));
	EntryMaps = LoadEntryMaps();
	UE_LOG(LogTestPlatform, Log,
		TEXT("panel gate: entry maps [%s]%s"),
		*FString::Join(EntryMaps, TEXT(", ")),
		bForceUI ? TEXT(", forced on by -TestPlatformForceUI") : TEXT(""));

	RuntimeHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FTestPlatformModule::TickRuntime), 0.f);
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FTestPlatformModule::TickSessionPoll), 0.2f);

	if (IConsoleManager::Get().FindConsoleObject(TEXT("TestPlatform.Panel")) == nullptr)
	{
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("TestPlatform.Panel"),
			TEXT("Show/hide the test platform UI (0 hides)."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				FTestPlatformModule* Module = GModuleInstance;
				if (!Module)
				{
					return;
				}
				const bool bShow = Args.Num() == 0 ? !Module->IsPanelShown() : Args[0] != TEXT("0");
				if (bShow)
				{
					Module->ShowPanel();
				}
				else
				{
					Module->HidePanel();
				}
			}));
	}
	// Same action as the 「启动后端」 button (the button calls StartBackendNow too),
	// exposed so the launch path can be exercised without a click.
	if (IConsoleManager::Get().FindConsoleObject(TEXT("TestPlatform.StartBackend")) == nullptr)
	{
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("TestPlatform.StartBackend"),
			TEXT("Start the Python backend in a visible terminal window if it is not already answering."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				FTestPlatformModule* Module = GModuleInstance;
				if (!Module)
				{
					return;
				}
				FString Error;
				if (Module->StartBackendNow(Error))
				{
					UE_LOG(LogTestPlatform, Log,
						TEXT("TestPlatform.StartBackend: probing /health before spawning %s"),
						*FTestPlatformModule::GetPanelUrl());
				}
				else
				{
					UE_LOG(LogTestPlatform, Warning,
						TEXT("TestPlatform.StartBackend: %s"), *Error);
				}
			}));
	}
}

void FTestPlatformModule::ShutdownModule()
{
	for (FTSTicker::FDelegateHandle* Handle : { &RuntimeHandle, &PollHandle })
	{
		if (Handle->IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(*Handle);
			Handle->Reset();
		}
	}
	if (UI.IsValid())
	{
		if (UGameViewportClient* ViewportClient = (GEngine ? GEngine->GameViewport : nullptr))
		{
			if (bPanelAdded)
			{
				ViewportClient->RemoveViewportWidgetContent(UI.ToSharedRef());
				bPanelAdded = false;
			}
		}
	}
	if (ACameraActor* Camera = CameraActor.Get())
	{
		Camera->Destroy();
	}
	CameraActor.Reset();
	UI.Reset();
	GModuleInstance = nullptr;
}

FString FTestPlatformModule::GetPanelUrl()
{
	return FTestPlatformApi::RootUrl();
}

// ---------------------------------------------------------------------- //
// UI hosting
// ---------------------------------------------------------------------- //
void FTestPlatformModule::EnsureUI(UGameViewportClient* ViewportClient)
{
	if (bPanelAdded || ViewportClient == nullptr)
	{
		return;
	}
	if (!UI.IsValid())
	{
		SAssignNew(UI, STestPlatformUI);
		UI->OnPan = [this](FVector2D Delta) { CameraRig.AddUserPan(Delta); };
		UI->OnZoom = [this](float Delta) { CameraRig.AddUserZoom(Delta); };
		UI->OnResetView = [this]() { CameraRig.ResetUserView(); };
		// The 「启动后端」 button and TestPlatform.StartBackend share this one path.
		UI->OnStartBackend = [this](FString& OutError) { return StartBackendNow(OutError); };
	}
	ViewportClient->AddViewportWidgetContent(UI.ToSharedRef(), 10);
	bPanelAdded = true;
	UE_LOG(LogTestPlatform, Log, TEXT("test platform UI added to the game viewport"));
}

void FTestPlatformModule::RemovePanelFromViewport()
{
	if (!UI.IsValid() || !bPanelAdded)
	{
		return;
	}
	UGameViewportClient* ViewportClient = nullptr;
	if (CachedViewportClient.IsValid())
	{
		ViewportClient = CachedViewportClient.Get();
	}
	else if (GEngine != nullptr)
	{
		ViewportClient = GEngine->GameViewport;
	}
	if (ViewportClient != nullptr)
	{
		ViewportClient->RemoveViewportWidgetContent(UI.ToSharedRef());
	}
	bPanelAdded = false;
	// The panel owned the cursor and GameAndUI input; give the map back so a
	// browse is not left with a dead mouse pointer.
	ApplyInputMode(false);
	UE_LOG(LogTestPlatform, Log, TEXT("test platform UI removed from the game viewport"));
}

FString FTestPlatformModule::WorldMapId(const UWorld* World)
{
	if (World == nullptr)
	{
		return FString();
	}
	const UPackage* Package = World->GetOutermost();
	return StripPiePrefix(Package != nullptr ? Package->GetName() : FString());
}

bool FTestPlatformModule::IsEntryMap(const FString& MapId) const
{
	if (MapId.IsEmpty())
	{
		return false;
	}
	FString ShortName = MapId;
	int32 Slash = INDEX_NONE;
	if (ShortName.FindLastChar(TEXT('/'), Slash))
	{
		ShortName.RightChopInline(Slash + 1);
	}
	for (const FString& Entry : EntryMaps)
	{
		const FString Candidate = Entry.TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			continue;
		}
		if (Candidate.Equals(MapId, ESearchCase::IgnoreCase)
			|| Candidate.Equals(ShortName, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// A path written without the /Game prefix still matches, as long as it
		// ends on a folder boundary.
		if (MapId.Len() > Candidate.Len() && MapId.EndsWith(Candidate, ESearchCase::IgnoreCase)
			&& MapId[MapId.Len() - Candidate.Len() - 1] == TEXT('/'))
		{
			return true;
		}
	}
	return false;
}

void FTestPlatformModule::ShowPanel()
{
	// An explicit request (TestPlatform.Panel) outranks the map gate until
	// TestPlatform.Panel 0: the manual equivalent of -TestPlatformForceUI. The
	// automatic path never shows the panel outside an entry-map run.
	bPanelManuallyShown = true;
	if (UGameViewportClient* ViewportClient = (GEngine ? GEngine->GameViewport : nullptr))
	{
		EnsureUI(ViewportClient);
	}
	if (UI.IsValid())
	{
		UI->SetUIVisible(true);
	}
	UE_LOG(LogTestPlatform, Log,
		TEXT("test platform UI shown by request on map %s (manual override of the entry-map gate)"),
		*LastGatedMapId);
}

void FTestPlatformModule::HidePanel()
{
	bPanelManuallyShown = false;
	if (UI.IsValid())
	{
		UI->SetUIVisible(false);
	}
}

void FTestPlatformModule::TogglePanel()
{
	if (IsPanelShown())
	{
		HidePanel();
	}
	else
	{
		ShowPanel();
	}
}

bool FTestPlatformModule::IsPanelShown() const
{
	return bPanelAdded && UI.IsValid() && UI->IsUIVisible();
}

// ---------------------------------------------------------------------- //
// backend auto-start
// ---------------------------------------------------------------------- //
FString FTestPlatformModule::BackendDir() const
{
	FString Dir = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Source/TestPlatform/Platform/backend"));
	FPaths::CollapseRelativeDirectories(Dir);
	return FPaths::ConvertRelativePathToFull(Dir);
}

FString FTestPlatformModule::PythonExe() const
{
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("TestPlatformPython="), Override)
		&& !Override.IsEmpty())
	{
		return Override;
	}
	if (FPaths::FileExists(DefaultPython))
	{
		return DefaultPython;
	}
	return TEXT("python.exe");
}

bool FTestPlatformModule::StartBackendNow(FString& OutError)
{
	OutError.Reset();
	FString Host;
	int32 Port = 0;
	if (!ParseBackendEndpoint(GetPanelUrl(), Host, Port))
	{
		OutError = FString::Printf(
			TEXT("无法启动后端：解析不了后端地址 %s"), *GetPanelUrl());
		return false;
	}
	if (!IsLoopbackHost(Host))
	{
		OutError = FString::Printf(
			TEXT("无法启动后端：%s 不是本机地址，不能在本机拉起 run.py"), *Host);
		return false;
	}
	// Fail here, while the UI can still show the reason, rather than opening a
	// terminal window that immediately prints a missing-file error.
	const FString Dir = BackendDir();
	if (!FPaths::FileExists(FPaths::Combine(Dir, TEXT("run.py"))))
	{
		OutError = FString::Printf(TEXT("无法启动后端：找不到 %s"), *FPaths::Combine(Dir, TEXT("run.py")));
		return false;
	}
	if (!FPaths::FileExists(PythonExe()))
	{
		OutError = FString::Printf(TEXT("无法启动后端：找不到解释器 %s"), *PythonExe());
		return false;
	}
	if (bBackendProbeInFlight)
	{
		OutError = TEXT("正在检测后端，请稍后再试");
		return false;
	}

	// Reuse the same liveness probe the automatic path uses. Its answer decides
	// whether a launch is needed at all, so an answering backend is never
	// duplicated even though this path bypasses the one-shot latch.
	bBackendProbeInFlight = true;
	bBackendProbeIsManual = true;
	BackendProbeRequest = FHttpModule::Get().CreateRequest();
	BackendProbeRequest->SetURL(GetPanelUrl() + TEXT("/health"));
	BackendProbeRequest->SetVerb(TEXT("GET"));
	BackendProbeRequest->SetTimeout(2.0f);
	BackendProbeRequest->OnProcessRequestComplete().BindRaw(
		this, &FTestPlatformModule::OnBackendProbeResponse);
	if (!BackendProbeRequest->ProcessRequest())
	{
		bBackendProbeInFlight = false;
		bBackendProbeIsManual = false;
		BackendProbeRequest.Reset();
		// The probe could not be queued at all. This is an explicit request, so
		// try the launch instead of failing: the window is the user's feedback.
		if (!SpawnBackend(Host, Port, /*bExplicit=*/true))
		{
			OutError = TEXT("无法启动后端：终端窗口没有创建成功，请查看日志");
			return false;
		}
	}
	return true;
}

void FTestPlatformModule::TryStartBackend()
{
	// Give the page/API a grace period to answer on its own, and never spawn a
	// second backend: the first one only needs a moment to bind the port.
	if (RuntimeSeconds < 10.0 || bBackendSpawnAttempted)
	{
		return;
	}
	if (CVarBackendAutoStart.GetValueOnGameThread() == 0)
	{
		return;
	}

	// The HTTP client, the probe and the spawned process must all agree on one
	// endpoint; -TestPlatformBackend= is the single place that changes it.
	FString Host;
	int32 Port = 0;
	if (!ParseBackendEndpoint(GetPanelUrl(), Host, Port))
	{
		bBackendSpawnAttempted = true;
		UE_LOG(LogTestPlatform, Warning,
			TEXT("cannot start backend: unparsable URL %s"), *GetPanelUrl());
		return;
	}
	if (!IsLoopbackHost(Host))
	{
		bBackendSpawnAttempted = true;
		UE_LOG(LogTestPlatform, Log,
			TEXT("backend URL %s is not local; not spawning run.py"), *GetPanelUrl());
		return;
	}
	if (bBackendProbeInFlight)
	{
		return;
	}

	// Ask the API itself (GET /health) instead of opening a raw socket. A socket
	// probe is the trap this replaces: UE reports a non-blocking connect's
	// EWOULDBLOCK as success, so the old probe answered "listening" whether or
	// not anything listened, and on this machine a blocking connect to a refused
	// loopback port only returns after ~2 s. The HTTP request is driven by the
	// HTTP module's own thread, so the game thread never waits for it.
	bBackendProbeInFlight = true;
	BackendProbeRequest = FHttpModule::Get().CreateRequest();
	BackendProbeRequest->SetURL(GetPanelUrl() + TEXT("/health"));
	BackendProbeRequest->SetVerb(TEXT("GET"));
	BackendProbeRequest->SetTimeout(2.0f);
	BackendProbeRequest->OnProcessRequestComplete().BindRaw(
		this, &FTestPlatformModule::OnBackendProbeResponse);
	if (!BackendProbeRequest->ProcessRequest())
	{
		// The probe could not even be queued: fail safe towards one spawn.
		bBackendProbeInFlight = false;
		BackendProbeRequest.Reset();
		SpawnBackend(Host, Port);
	}
}

void FTestPlatformModule::OnBackendProbeResponse(
	FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	const bool bExplicit = bBackendProbeIsManual;
	bBackendProbeInFlight = false;
	bBackendProbeIsManual = false;
	BackendProbeRequest.Reset();

	// Any HTTP answer means the port is taken; a second run.py would only die
	// with "address already in use". No answer (refused, timeout) means start one.
	if (bConnectedSuccessfully && Response.IsValid())
	{
		bBackendSpawnAttempted = true;
		UE_LOG(LogTestPlatform, Log,
			TEXT("%s backend already answering on %s (GET /health -> %d), not spawning another"),
			bExplicit ? TEXT("explicit start requested but") : TEXT("backend"),
			*GetPanelUrl(), Response->GetResponseCode());
		return;
	}

	FString Host;
	int32 Port = 0;
	if (!ParseBackendEndpoint(GetPanelUrl(), Host, Port) || !IsLoopbackHost(Host))
	{
		bBackendSpawnAttempted = true;
		return;
	}
	UE_LOG(LogTestPlatform, Log,
		TEXT("no /health answer on %s; spawning run.py (%s)"),
		*GetPanelUrl(), bExplicit ? TEXT("explicit start") : TEXT("automatic start"));
	if (!SpawnBackend(Host, Port, bExplicit) && bExplicit)
	{
		UE_LOG(LogTestPlatform, Error,
			TEXT("explicit backend start failed: the terminal window could not be created"));
	}
}

FString FTestPlatformModule::TerminalLauncher() const
{
	// Testing hook: point -TestPlatformBackendLauncher= at a missing file and the
	// console fallback runs instead. An override that does not exist is treated as
	// "no launcher" rather than being launched blindly.
	FString Override;
	if (FParse::Value(
			FCommandLine::Get(), TEXT("TestPlatformBackendLauncher="), Override)
		&& !Override.IsEmpty())
	{
		if (FPaths::FileExists(Override))
		{
			return Override;
		}
		UE_LOG(LogTestPlatform, Warning,
			TEXT("backend launcher override %s does not exist"), *Override);
		return FString();
	}
	// Windows Terminal is installed per user through the Store, so the app
	// execution alias under LOCALAPPDATA is the path that does not depend on PATH.
	const FString LocalAppData =
		FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		const FString Alias = FPaths::Combine(
			LocalAppData, TEXT("Microsoft/WindowsApps/wt.exe"));
		if (FPaths::FileExists(Alias))
		{
			return Alias;
		}
	}
	return TEXT("wt.exe"); // PATH lookup; CreateProc reports the failure if it is absent too
}

bool FTestPlatformModule::SpawnBackend(const FString& Host, int32 Port, bool bExplicit)
{
	if (!bExplicit)
	{
		if (bBackendSpawnAttempted)
		{
			return false;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - LastBackendAttempt < 8.0)
		{
			return false; // rate limit only: the poller comes back in a moment
		}
		if (FPlatformProcess::IsProcRunning(BackendProcess))
		{
			return false;
		}
	}
	// An explicit start (button / console command) bypasses the one-shot latch
	// and the rate limit, but the caller has already probed /health: a backend
	// that answers is never duplicated.
	LastBackendAttempt = FPlatformTime::Seconds();
	const FString Dir = BackendDir();
	const FString Script = FPaths::Combine(Dir, TEXT("run.py"));
	const FString Exe = PythonExe();
	if (!FPaths::FileExists(Script) || !FPaths::FileExists(Exe))
	{
		UE_LOG(LogTestPlatform, Warning,
			TEXT("cannot start backend: script=%s exe=%s"), *Script, *Exe);
		return false;
	}
	// run.py reads these at import time; the spawned process inherits them, so
	// the backend it starts binds to the same endpoint the HTTP client uses.
	FPlatformMisc::SetEnvironmentVar(TEXT("TESTPLATFORM_HOST"), *Host);
	FPlatformMisc::SetEnvironmentVar(TEXT("TESTPLATFORM_PORT"), *FString::FromInt(Port));

	// One shot for the automatic path. The latch is taken before either launch
	// so a manual close can never be answered with a second window: nothing
	// below retries, and the poller stops calling SpawnBackend once this flag is
	// set. An explicit user request is the one caller allowed past the latch.
	bBackendSpawnAttempted = true;

	// The backend is deliberately visible now: the operator watches its log live
	// and stops it by closing the window. So every launch here is non-hidden, and
	// the editor holds no job object over it - closing the editor therefore leaves
	// the window (and the backend) running under the operator's control. The
	// editor's ShutdownModule does not terminate it either.
	//
	// Window geometry and quoting, all of which were prototyped by hand:
	//   * -w -1 asks Windows Terminal for a window of its own. Without it the tab
	//     lands in whatever window happens to be open, and "close the window"
	//     would take the operator's other tabs with it.
	//   * --suppressApplicationTitle pins the title so neither cmd nor python can
	//     rename the window that is supposed to be recognisable.
	//   * cmd /k keeps the window open after python exits, so a traceback stays
	//     readable instead of vanishing with the process.
	//   * wt re-tokenises this command line before handing it to the child, so a
	//     nested quote around the interpreter does not survive. The interpreter
	//     path must therefore stay free of spaces (the conda path is); a python
	//     path with spaces needs a wrapper script instead.
	uint32 ProcessId = 0;
	const FString Launcher = TerminalLauncher();
	if (!Launcher.IsEmpty())
	{
		const FString Params = FString::Printf(
			TEXT("-w -1 nt --title \"%s\" --suppressApplicationTitle -d \"%s\" cmd /k \"chcp 65001>nul && \"%s\" run.py\""),
			BackendWindowTitle, *Dir, *Exe);
		BackendProcess = FPlatformProcess::CreateProc(
			*Launcher, *Params, /*Detached*/true, /*Hidden*/false, /*ReallyHidden*/false,
			&ProcessId, 0, *Dir, nullptr, nullptr);
		if (BackendProcess.IsValid())
		{
			UE_LOG(LogTestPlatform, Log,
				TEXT("backend window opened via Windows Terminal %s (launcher pid %u): title \"%s\", dir %s, endpoint %s:%d, command \"%s\" run.py"),
				*Launcher, ProcessId, BackendWindowTitle, *Dir, *Host, Port, *Exe);
			return true;
		}
		UE_LOG(LogTestPlatform, Warning,
			TEXT("could not launch %s for the backend window; falling back to a plain console"),
			*Launcher);
	}
	else
	{
		UE_LOG(LogTestPlatform, Warning,
			TEXT("no Windows Terminal launcher available; falling back to a plain console"));
	}

	// Fallback: a plain visible console window with the same fixed title and the
	// same live output. `start` is what gives it a window of its own; its first
	// quoted argument is the title and the rest is the command. Never hidden.
	const FString Shell = ConsoleShell();
	const FString Params = FString::Printf(
		TEXT("/c start \"%s\" \"%s\" run.py"), BackendWindowTitle, *Exe);
	BackendProcess = FPlatformProcess::CreateProc(
		*Shell, *Params, /*Detached*/true, /*Hidden*/false, /*ReallyHidden*/false,
		&ProcessId, 0, *Dir, nullptr, nullptr);
	if (!BackendProcess.IsValid())
	{
		UE_LOG(LogTestPlatform, Error,
			TEXT("cannot start backend: neither Windows Terminal nor %s launched"), *Shell);
		return false;
	}
	UE_LOG(LogTestPlatform, Log,
		TEXT("backend window opened via plain console %s (launcher pid %u): title \"%s\", dir %s, endpoint %s:%d, command \"%s\" run.py"),
		*Shell, ProcessId, BackendWindowTitle, *Dir, *Host, Port, *Exe);
	return true;
}

// ---------------------------------------------------------------------- //
// per-frame runtime work
// ---------------------------------------------------------------------- //
bool FTestPlatformModule::TickRuntime(float DeltaTime)
{
	if (GEngine == nullptr)
	{
		return true;
	}
	UGameViewportClient* ViewportClient = GEngine->GameViewport;
	if (ViewportClient == nullptr)
	{
		return true;
	}
	UWorld* World = ViewportClient->GetWorld();
	if (World == nullptr || World->WorldType == EWorldType::Editor)
	{
		return true;
	}

	// The panel is the front end of an entry level, not an overlay on every map:
	// browsing a town must stay clean. The verdict is latched for the run
	// because the backend's preview load (client.load_world) replaces the entry
	// level with the selected town while the config screen has to stay up --
	// gating on the current map alone would hide the panel exactly when the
	// city appears.
	const FString MapId = WorldMapId(World);
	const bool bOnEntryMap = IsEntryMap(MapId);
	bEntryMapArmed = bEntryMapArmed || bOnEntryMap;
	const bool bAllowed = bForceUI || bEntryMapArmed || bPanelManuallyShown;
	if (bAllowed != bPanelAllowed || !MapId.Equals(LastGatedMapId))
	{
		UE_LOG(LogTestPlatform, Log, TEXT("map %s: config panel %s (%s)"),
			*MapId, bAllowed ? TEXT("enabled") : TEXT("disabled"),
			bForceUI ? TEXT("-TestPlatformForceUI")
				: bOnEntryMap ? TEXT("entry map")
				: bEntryMapArmed ? TEXT("this run started on an entry map")
				: bPanelManuallyShown ? TEXT("TestPlatform.Panel")
				: TEXT("not an entry map"));
		bPanelAllowed = bAllowed;
		LastGatedMapId = MapId;
		GLog->Flush();
	}
	if (!bAllowed)
	{
		// Clean browse: no widget, no preview camera, no input capture and no
		// screen-message suppression in a map that is not the entry level.
		RemovePanelFromViewport();
		return true;
	}

	// A map load (client.load_world) rebuilds the viewport client and the player
	// controller; the widget and the input mode have to be re-bound or the UI
	// stays visible but stops receiving clicks.
	bool bRebind = false;
	if (CachedViewportClient.Get() != ViewportClient)
	{
		if (UI.IsValid() && bPanelAdded && CachedViewportClient.IsValid())
		{
			CachedViewportClient->RemoveViewportWidgetContent(UI.ToSharedRef());
			bPanelAdded = false;
		}
		CachedViewportClient = ViewportClient;
		bRebind = true;
	}
	EnsureUI(ViewportClient);

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (PlayerController == nullptr)
	{
		return true;
	}
	if (PlayerControllerPtr.Get() != PlayerController)
	{
		PlayerControllerPtr = PlayerController;
		bRebind = true;
	}
	EnsureCamera(World, PlayerController);

	RuntimeSeconds += DeltaTime;
	GAreScreenMessagesEnabled = false; // the platform owns the screen
	CameraRig.Tick(DeltaTime);
	ApplyCameraPose();

	const bool bWantUI = UI.IsValid() && UI->IsUIVisible();
	if (bRebind)
	{
		bUICaptured = !bWantUI; // force the input mode to be applied again
		ApplyInputMode(bWantUI);
	}
	else if (bWantUI != bUICaptured)
	{
		ApplyInputMode(bWantUI);
	}
	else if (bWantUI && RuntimeSeconds - LastInputAssert > 1.5)
	{
		LastInputAssert = RuntimeSeconds; // keep the cursor/slate focus honest
		ApplyInputMode(true);
	}
	return true;
}

void FTestPlatformModule::EnsureCamera(UWorld* World, APlayerController* PlayerController)
{
	ACameraActor* Camera = CameraActor.Get();
	if (Camera == nullptr)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Name = TEXT("TestPlatformCamera");
		Camera = World->SpawnActor<ACameraActor>(
			ACameraActor::StaticClass(), FTransform::Identity, Params);
		if (Camera == nullptr)
		{
			return;
		}
		if (UCameraComponent* CameraComponent = Camera->GetCameraComponent())
		{
			CameraComponent->SetFieldOfView(90.f);
			CameraComponent->bConstrainAspectRatio = false;
		}
		CameraActor = Camera;
		UE_LOG(LogTestPlatform, Log, TEXT("spawned runtime preview camera"));
	}
	// CARLA drives the view through the possessed spectator pawn, so the view
	// target is left alone (setting it just gets overridden every frame).
}

void FTestPlatformModule::ApplyCameraPose()
{
	ACameraActor* Camera = CameraActor.Get();
	if (Camera == nullptr || !CameraRig.HasCurrentPose())
	{
		return; // no session yet: leave the engine's own camera alone
	}
	const FVector Pose = CameraRig.GetCurrentLocation();
	const FRotator PoseRot = CameraRig.GetCurrentRotation();
	Camera->SetActorLocationAndRotation(Pose, PoseRot);

	// The rendered view follows the spectator pawn that the player controller
	// possesses, so that is what has to move. Teleporting it is what CARLA's own
	// spectator API does under the hood.
	if (APlayerController* PlayerController = PlayerControllerPtr.Get())
	{
		if (APawn* Spectator = PlayerController->GetPawn())
		{
			Spectator->SetActorLocationAndRotation(Pose, PoseRot);
		}
		else if (PlayerController->GetViewTarget() != Camera)
		{
			PlayerController->SetViewTargetWithBlend(Camera, 0.f);
		}
	}
}

void FTestPlatformModule::ApplyInputMode(bool bCaptureForUI)
{
	APlayerController* PlayerController = PlayerControllerPtr.Get();
	if (PlayerController == nullptr)
	{
		return;
	}
	if (bCaptureForUI)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PlayerController->SetInputMode(Mode);
		PlayerController->bShowMouseCursor = true;
	}
	else
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->bShowMouseCursor = false;
	}
	bUICaptured = bCaptureForUI;
}

// ---------------------------------------------------------------------- //
// session polling
// ---------------------------------------------------------------------- //
bool FTestPlatformModule::TickSessionPoll(float DeltaTime)
{
	static double LastReadyCheck = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (UI.IsValid() && (Now - LastReadyCheck) > 2.0)
	{
		LastReadyCheck = Now;
		if (!UI->IsBackendReady())
		{
			TryStartBackend();
		}
	}
	RequestSessionState();
	return true;
}

void FTestPlatformModule::RequestSessionState()
{
	if (bPollInFlight)
	{
		return;
	}
	bPollInFlight = true;
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FTestPlatformModule::GetPanelUrl() + TEXT("/sessions/current"));
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(2.0f);
	Request->OnProcessRequestComplete().BindRaw(
		this, &FTestPlatformModule::OnSessionStateResponse);
	PollRequest = Request;
	if (!Request->ProcessRequest())
	{
		bPollInFlight = false;
	}
}

void FTestPlatformModule::OnSessionStateResponse(
	FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bPollInFlight = false;
	if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		// Deliberately keep the UI's in-flight flag as it is: a missed poll (the
		// panel itself is frozen during a map load) is not evidence that the load
		// ended. It clears on the next real answer, or the chip falls back to
		// 未知 on its own because /health stops answering as well.
		return;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}
	FString State = TEXT("none");
	Root->TryGetStringField(TEXT("state"), State);
	HandleSessionState(State, Root);
}

void FTestPlatformModule::HandleSessionState(
	const FString& State, const TSharedPtr<FJsonObject>& Root)
{
	const TSharedPtr<FJsonObject>* Cameras = nullptr;
	if (Root->TryGetObjectField(TEXT("cameras"), Cameras) && Cameras && Cameras->IsValid())
	{
		auto ReadPose = [&](const TCHAR* Key, FVector& OutLocation, FRotator& OutRotation) -> bool
		{
			const TSharedPtr<FJsonObject>* Pose = nullptr;
			if (!(*Cameras)->TryGetObjectField(Key, Pose) || Pose == nullptr || !Pose->IsValid())
			{
				return false;
			}
			double X = 0.0, Y = 0.0, Z = 0.0, Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
			(*Pose)->TryGetNumberField(TEXT("x"), X);
			(*Pose)->TryGetNumberField(TEXT("y"), Y);
			(*Pose)->TryGetNumberField(TEXT("z"), Z);
			(*Pose)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*Pose)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*Pose)->TryGetNumberField(TEXT("roll"), Roll);
			OutLocation = FVector(X, Y, Z);
			OutRotation = FRotator(Pitch, Yaw, Roll);
			return true;
		};

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		if (ReadPose(TEXT("preview"), Location, Rotation))
		{
			CameraRig.SetPreviewPose(Location, Rotation);
		}
		if (ReadPose(TEXT("follow"), Location, Rotation))
		{
			CameraRig.SetFollowPose(Location, Rotation);
		}
	}

	if (State != LastSessionState)
	{
		const FString Previous = LastSessionState;
		LastSessionState = State;

		if (State == TEXT("running"))
		{
			if (UI.IsValid())
			{
				UI->SetUIVisible(false);
			}
			CameraRig.EnterFollow(0.9f);
		}
		else if (State == TEXT("preview"))
		{
			if (UI.IsValid())
			{
				UI->SetUIVisible(true);
			}
			CameraRig.EnterPreview(0.9f);
		}
		else if (State == TEXT("stopped") || State == TEXT("finished") || State == TEXT("error"))
		{
			if (Previous == TEXT("running") || Previous == TEXT("preview"))
			{
				if (UI.IsValid())
				{
					UI->SetUIVisible(true);
				}
				CameraRig.EnterPreview(1.0f);
			}
		}
	}

	// feed the frame so its top bar / status line follow the real backend state
	if (UI.IsValid())
	{
		FSessionStatus Status;
		Status.bValid = Root.IsValid();
		Root->TryGetStringField(TEXT("id"), Status.Id);
		Root->TryGetStringField(TEXT("state"), Status.State);
		Root->TryGetStringField(TEXT("error"), Status.Error);
		Root->TryGetStringField(TEXT("map"), Status.MapId);
		double Tick = 0.0, Events = 0.0, SimTime = 0.0;
		if (Root->TryGetNumberField(TEXT("tick"), Tick)) { Status.Tick = static_cast<int32>(Tick); }
		if (Root->TryGetNumberField(TEXT("events"), Events)) { Status.Events = static_cast<int32>(Events); }
		if (Root->TryGetNumberField(TEXT("sim_time"), SimTime)) { Status.SimTime = static_cast<float>(SimTime); }
		// The clock the session is really running at: the sync chip is painted
		// from this, so it cannot keep claiming a fixed delta the simulator has
		// dropped.
		bool bSyncMode = false;
		Status.bSyncMode = Root->TryGetBoolField(TEXT("sync_mode"), bSyncMode) && bSyncMode;
		bool bSyncLost = false;
		Status.bSyncLost = Root->TryGetBoolField(TEXT("sync_lost"), bSyncLost) && bSyncLost;
		double FixedDelta = 0.0;
		if (Root->TryGetNumberField(TEXT("fixed_delta"), FixedDelta))
		{
			Status.FixedDelta = static_cast<float>(FixedDelta);
		}
		Root->TryGetStringField(TEXT("sync_lost_detail"), Status.SyncLostDetail);
		const TSharedPtr<FJsonObject>* Ego = nullptr;
		if (Root->TryGetObjectField(TEXT("ego"), Ego) && Ego && Ego->IsValid())
		{
			double Speed = 0.0;
			if ((*Ego)->TryGetNumberField(TEXT("speed_kmh"), Speed))
			{
				Status.SpeedKmh = static_cast<float>(Speed);
			}
		}
		UI->SetSessionStatus(Status);
	}
}

IMPLEMENT_MODULE(FTestPlatformModule, TestPlatform)

#undef LOCTEXT_NAMESPACE










