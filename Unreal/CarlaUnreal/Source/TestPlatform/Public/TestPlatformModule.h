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
	/** Takes the panel off the game viewport and hands the input back to the map. */
	void RemovePanelFromViewport();
	void EnsureCamera(UWorld* World, APlayerController* PlayerController);
	void ApplyCameraPose();
	void ApplyInputMode(bool bUICaptured);

	/**
	 * Map gate. The panel only exists in an entry-map run: the whitelist comes
	 * from UTestPlatformSettings (DefaultGame.ini), -TestPlatformForceUI forces
	 * it on everywhere, and an explicit TestPlatform.Panel is a manual override.
	 */
	bool IsEntryMap(const FString& MapId) const;
	/** Map package name of a world, with any PIE prefix removed. */
	static FString WorldMapId(const UWorld* World);
	/** True when MapId is the town the panel itself asked the backend to load. */
	bool IsBackendPreviewMap(const FString& MapId) const;
	/**
	 * Forgets ExpectedPreviewMap when its request can no longer deliver a world
	 * change (the request failed, or the session ended in error), so a later
	 * manual load of exactly that town is not mistaken for the live preview.
	 */
	void ClearExpectedPreview(const TCHAR* Why);
	/** Forgets the per-run gate state when the game/PIE session is gone. */
	void ResetGateForNewSession();

	void TryStartBackend();

	/**
	 * Starts the backend on an explicit user request (「启动后端」 button and the
	 * `TestPlatform.StartBackend` console command). Probes /health first, so a
	 * backend that is already answering is never duplicated; unlike the automatic
	 * path it bypasses the one-shot latch and the 8 s rate limit. Returns false
	 * (and OutError) only when the request cannot be issued at all.
	 */
	bool StartBackendNow(FString& OutError);

	/** Completion of the GET /health probe that decides whether run.py is needed. */
	void OnBackendProbeResponse(FHttpRequestPtr Request,
		FHttpResponsePtr Response, bool bConnectedSuccessfully);
	/** bExplicit is the user-requested launch: it bypasses latch and rate limit. */
	bool SpawnBackend(const FString& Host, int32 Port, bool bExplicit = false);
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

	// ---- map gate (see IsEntryMap / UTestPlatformSettings) ----
	/** Whitelist read once at startup from UTestPlatformSettings. */
	TArray<FString> EntryMaps;
	/** The map the gate last ruled on, for one log line per map instead of per frame. */
	FString LastGatedMapId;
	/** Last gate verdict; starts true so the first frame logs its decision. */
	bool bPanelAllowed = true;
	/** -TestPlatformForceUI on the command line: the panel ignores the gate. */
	bool bForceUI = false;
	/**
	 * The map of this game/PIE session's first world. Set once and never
	 * rewritten: it is the only map that can arm the panel, so a stray
	 * load_world into an entry map later in the run cannot arm it.
	 */
	FString RunStartMapId;
	/** Map package of the runtime world seen on the previous tick (world-change detection). */
	FString LastRuntimeMapId;
	/** This run started on an entry map and has not left the entry flow. */
	bool bEntryMapArmed = false;
	/** The start map was seen, so the gate has decided for this run. */
	bool bGateDecided = false;
	/** The run's arm was dropped by a world change that was not a backend preview. */
	bool bEntryFlowLeft = false;
	/**
	 * The map that ended the entry flow (the first world change the panel did
	 * not request); set once with bEntryFlowLeft and never rewritten, so the
	 * per-map log line names it instead of whatever map the run is on now.
	 */
	FString EntryFlowLeftMapId;
	/**
	 * Map the panel asked the backend to load (STestPlatformUI::OnPreviewRequest).
	 * That world change is the one exception an entry run keeps its panel through;
	 * it is cleared once the matching world change has been observed.
	 */
	FString ExpectedPreviewMap;
	/** TestPlatform.Panel 1: an explicit request outranks the gate until Panel 0. */
	bool bPanelManuallyShown = false;
	double LastBackendAttempt = -100.0;
	double RuntimeSeconds = 0.0;
	double LastInputAssert = 0.0;
	bool bBackendSpawnAttempted = false;
	bool bBackendProbeInFlight = false;
	/** the in-flight probe belongs to an explicit start (spawns on "no answer") */
	bool bBackendProbeIsManual = false;
	FProcHandle BackendProcess;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> PollRequest;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> BackendProbeRequest;
};



