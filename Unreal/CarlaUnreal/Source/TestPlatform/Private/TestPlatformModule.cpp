#include "TestPlatformModule.h"

#include "STestPlatformUI.h"
#include "TestPlatformApi.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogTestPlatform, Log, All);

#define LOCTEXT_NAMESPACE "TestPlatformModule"

namespace
{
	FTestPlatformModule* GModuleInstance = nullptr;
	const TCHAR* DefaultPython = TEXT("C:/SoftWare/MiniConda/envs/chat_carla/python.exe");

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
}

void FTestPlatformModule::StartupModule()
{
	UE_LOG(LogTestPlatform, Log, TEXT("CARLA test platform runtime module started"));
	GModuleInstance = this;

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
	}
	ViewportClient->AddViewportWidgetContent(UI.ToSharedRef(), 10);
	bPanelAdded = true;
	UE_LOG(LogTestPlatform, Log, TEXT("test platform UI added to the game viewport"));
}

void FTestPlatformModule::ShowPanel()
{
	if (UGameViewportClient* ViewportClient = (GEngine ? GEngine->GameViewport : nullptr))
	{
		EnsureUI(ViewportClient);
	}
	if (UI.IsValid())
	{
		UI->SetUIVisible(true);
	}
}

void FTestPlatformModule::HidePanel()
{
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
	bBackendProbeInFlight = false;
	BackendProbeRequest.Reset();

	// Any HTTP answer means the port is taken; a second run.py would only die
	// with "address already in use". No answer (refused, timeout) means start one.
	if (bConnectedSuccessfully && Response.IsValid())
	{
		bBackendSpawnAttempted = true;
		UE_LOG(LogTestPlatform, Log,
			TEXT("backend already answering on %s (GET /health -> %d), not spawning another"),
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
		TEXT("no /health answer on %s; spawning run.py once"), *GetPanelUrl());
	SpawnBackend(Host, Port);
}

void FTestPlatformModule::SpawnBackend(const FString& Host, int32 Port)
{
	if (bBackendSpawnAttempted)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now - LastBackendAttempt < 8.0)
	{
		return; // rate limit only: the poller comes back in a moment
	}
	LastBackendAttempt = Now;
	if (FPlatformProcess::IsProcRunning(BackendProcess))
	{
		return;
	}
	const FString Dir = BackendDir();
	const FString Script = FPaths::Combine(Dir, TEXT("run.py"));
	const FString Exe = PythonExe();
	if (!FPaths::FileExists(Script) || !FPaths::FileExists(Exe))
	{
		UE_LOG(LogTestPlatform, Warning,
			TEXT("cannot start backend: script=%s exe=%s"), *Script, *Exe);
		return;
	}
	// run.py reads these at import time; the spawned process inherits them, so
	// the backend it starts binds to the same endpoint the HTTP client uses.
	FPlatformMisc::SetEnvironmentVar(TEXT("TESTPLATFORM_HOST"), *Host);
	FPlatformMisc::SetEnvironmentVar(TEXT("TESTPLATFORM_PORT"), *FString::FromInt(Port));
	uint32 ProcessId = 0;
	const FString Params = FString::Printf(TEXT("\"%s\""), *Script);
	bBackendSpawnAttempted = true;
	BackendProcess = FPlatformProcess::CreateProc(
		*Exe, *Params, true, true, true, &ProcessId, 0, *Dir, nullptr, nullptr);
	UE_LOG(LogTestPlatform, Log, TEXT("backend spawn requested (pid %u) from %s on %s:%d"),
		ProcessId, *Dir, *Host, Port);
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










