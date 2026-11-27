#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/ModuleInterface.h"
#include "PreviewCameraRig.h"

class STestPlatformUI;
class FJsonObject;
class ACameraActor;
class APlayerController;
class UGameViewportClient;
class UWorld;

/**
 * Runtime module of the CARLA test platform (ships in packaged builds).
 *
 * Hosts the native configuration UI on the game viewport, owns the runtime
 * camera actor, starts the Python backend in a window the operator can watch and
 * close, and turns session state changes into the continuous camera move from the
 * map overview into the driving ego.
 */
class FTestPlatformModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void ShowPanel();
	void HidePanel();
	void TogglePanel();
	bool IsPanelShown() const;

	/** Backend root URL; resolves -TestPlatformBackend= (see FTestPlatformApi::RootUrl). */
	static FString GetPanelUrl();

private:
	bool TickRuntime(float DeltaTime);
	bool TickSessionPoll(float DeltaTime);

	void EnsureUI(UGameViewportClient* ViewportClient);
	void EnsureCamera(UWorld* World, APlayerController* PlayerController);
	void ApplyCameraPose();
	void ApplyInputMode(bool bUICaptured);

	void TryStartBackend();

	/** Completion of the GET /health probe that decides whether run.py is needed. */
	void OnBackendProbeResponse(FHttpRequestPtr Request,
		FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void SpawnBackend(const FString& Host, int32 Port);
	FString BackendDir() const;
	FString PythonExe() const;

	/** wt.exe (or the -TestPlatformBackendLauncher= override) for the visible
	 *  backend window; empty when no usable launcher exists. */
	FString TerminalLauncher() const;

	void RequestSessionState();
	void OnSessionStateResponse(FHttpRequestPtr Request,
		FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void HandleSessionState(const FString& State, const TSharedPtr<FJsonObject>& Root);

	TSharedPtr<STestPlatformUI> UI;
	TWeakObjectPtr<ACameraActor> CameraActor;
	TWeakObjectPtr<APlayerController> PlayerControllerPtr;
	TWeakObjectPtr<UGameViewportClient> CachedViewportClient;
	FTSTicker::FDelegateHandle RuntimeHandle;
	FTSTicker::FDelegateHandle PollHandle;

	FPreviewCameraRig CameraRig;
	FString LastSessionState = TEXT("none");
	bool bPollInFlight = false;
	bool bPanelAdded = false;
	bool bUICaptured = false;
	double LastBackendAttempt = -100.0;
	double RuntimeSeconds = 0.0;
	double LastInputAssert = 0.0;
	bool bBackendSpawnAttempted = false;
	bool bBackendProbeInFlight = false;
	FProcHandle BackendProcess;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> PollRequest;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> BackendProbeRequest;
};



