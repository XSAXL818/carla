#pragma once

#include "CoreMinimal.h"
#include "TestPlatformApi.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SVerticalBox;
class SWrapBox;
class SUniformGridPanel;
class STextBlock;
class SBox;
class SEditableTextBox;
class SScrollBox;
class SBorder;
struct FSlateBrush;

/**
 * Native configuration UI, laid out like the HTML prototype:
 *
 *   +--------------------------------------------------------------+
 *   | brand   [server][state][backend][sync]        [preset][enter] |  62 px
 *   +---------+--------------------------------------+-------------+
 *   | (1)地图 |  (3)主车路线 chips                  | (5)智驾算法 |
 *   | thumbs  |                                      | (6)运行设置 |
 *   | cards   |      live 3D world (nothing drawn)    | 本次配置    |
 *   |         |  (2)主车类型  (4)天气                 |             |
 *   +---------+--------------------------------------+-------------+
 *   | status hint                                    [enter world]  |
 *   +--------------------------------------------------------------+
 *
 * The middle is intentionally empty: the engine's render shows through.
 */
class STestPlatformUI : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(STestPlatformUI)
	{}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetUIVisible(bool bVisible);
	bool IsUIVisible() const { return bUIVisible; }
	void SetSessionStatus(const FSessionStatus& Status);
	/**
	 * Is a map load in flight according to the backend? GET /sessions/current
	 * reports state=preview_loading for the whole 30-90 s a load_world blocks the
	 * simulator, which is the window in which GET /health cannot answer honestly.
	 * The backend sets it, so a real answer clears it; a missed poll leaves it
	 * alone (the chip falls back to 未知 while /health is silent anyway).
	 */
	void SetSessionLoadInFlight(bool bInFlight);
	void ReloadCatalog();
	bool IsBackendReady() const;
	/** GET /health is currently answering: the backend process is up. */
	bool IsBackendReachable() const;
	bool IsLoadingMap() const { return bLoadingMap; }

	FPlatformSelection& Selection() { return Config; }

	/** Overview camera control, bound by the module to the camera rig. */
	TFunction<void(FVector2D)> OnPan;
	TFunction<void(float)> OnZoom;
	TFunction<void()> OnResetView;
	/**
	 * Starts the Python backend on the user's explicit request (the 「启动后端」
	 * button). Bound by the module to the same launch path the automatic start
	 * uses. Returns false and fills OutError when the launch cannot even be
	 * requested; success means "the request was issued", not "python is up" —
	 * the callbacks then wait for /health and the catalog.
	 */
	TFunction<bool(FString& /*OutError*/)> OnStartBackend;

private:
	// ---- construction
	TSharedRef<SWidget> MakeTopBar();
	TSharedRef<SWidget> MakeLeftColumn();
	TSharedRef<SWidget> MakeRightColumn();
	TSharedRef<SWidget> MakeCenterOverlays();
	TSharedRef<SWidget> MakeBottomBar();
	TSharedRef<SWidget> MakeLoadingOverlay();
	TSharedRef<SWidget> MakeViewportCatcher();

	TSharedRef<SWidget> NumberedTitle(const FString& Number, const FString& Title,
		const FString& Aside = FString());
	TSharedRef<SWidget> Card(TSharedRef<SWidget> Body);
	TSharedRef<SWidget> Chip(const FString& Text, bool bSelected, TFunction<void()> OnClicked,
		const FString& Tooltip = FString());
	TSharedRef<SWidget> MapCard(const FMapInfo& Map, bool bSelected, TFunction<void()> OnClicked);
	TSharedRef<SWidget> VehicleCard(const FVehicleInfo& Vehicle, bool bSelected,
		TFunction<void()> OnClicked);
	TSharedRef<SWidget> SliderRow(const FString& Label, float Min, float Max, float Step,
		float& Value, const FString& Unit);
	TSharedRef<SWidget> CheckRow(const FString& Label, bool& Flag);
	TSharedRef<SWidget> KV(const FString& Key, const FString& Value);

	// ---- data
	void RebuildAll();
	void RebuildMapRows();
	void RebuildRouteChips();
	void RebuildVehicles();
	void RebuildWeather();
	void RebuildAgent();
	void RebuildRunSettings();
	void RebuildSummary();
	void UpdateChrome();
	EActiveTimerReturnType TickRefresh(double Time, float DeltaTime);

	// ---- backend card (state follows the live /health answer, see UpdateChrome)
	FText OfflineTitle() const;
	FText OfflineHint() const;
	EVisibility OfflineCardVisibility() const;
	EVisibility OfflineErrorVisibility() const;
	EVisibility StartBackendButtonVisibility() const;
	bool IsStartBackendEnabled() const;
	FText StartBackendLabel() const;
	void RequestBackendStart();

	/** True while a load_world is in flight: the /health probe cannot tell then. */
	bool IsCarlaProbeBlocked() const { return bLoadingMap || bSessionLoadInFlight; }
	/**
	 * Keep the probe window and the chip in step when it opens or closes: on
	 * open, drop the last /health verdict; on close, ask again immediately (and
	 * force a re-probe), instead of leaving whatever the blocked window produced.
	 */
	void SyncCarlaProbeWindow();

	// ---- behaviour
	void OnSelectMap(const FString& MapId);
	void OnPrimaryClicked();
	void BeginLoadAndStart();
	void LoadSelectedMap(TFunction<void(bool)> Done);
	void StartSessionNow();
	void StopSessionNow();
	void SetStatus(const FString& Text, bool bWarn = false);

	const FSlateBrush* Thumb(const FString& Url, const FVector2D& DrawSize);
	void FetchThumb(const FString& Key, const FVector2D& DrawSize);

	FString StateText(const FString& State) const;
	FLinearColor StateColor() const;
	FString PresetLabel(const FMapPreset& Preset) const;

private:
	TSharedPtr<FTestPlatformApi> Api;
	FPlatformSelection Config;
	FSessionStatus Session;

	TSharedPtr<SVerticalBox> MapRowsBox;
	TSharedPtr<SVerticalBox> RouteChipsBox;
	TSharedPtr<SWrapBox> VehicleGrid;
	TSharedPtr<SWrapBox> WeatherGrid;
	TSharedPtr<SVerticalBox> AgentBox;
	TSharedPtr<SVerticalBox> RunBox;
	TSharedPtr<SVerticalBox> SummaryBox;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> SessionChipText;
	TSharedPtr<STextBlock> ServerChipText;
	/** the square in front of ServerChipText; painted from the same /health state */
	TSharedPtr<SBorder> ServerDot;
	/** 同步模式 chip: the clock the backend reports, never a constant */
	TSharedPtr<STextBlock> SyncChipText;
	TSharedPtr<SBox> LoadingBox;
	TSharedPtr<STextBlock> LoadingText;
	TSharedPtr<SEditableTextBox> SearchBox;
	TSharedPtr<STextBlock> PrimaryTopLabel;
	TSharedPtr<STextBlock> PrimaryBottomLabel;
	TSharedPtr<STextBlock> MapChipText;
	TSharedPtr<STextBlock> WeatherChipText;
	TSharedPtr<STextBlock> CaptionText;
	TSharedPtr<STextBlock> SpeedValueText;

	TMap<FString, TSharedPtr<FSlateBrush>> ThumbCache;
	/** Rooted so GC cannot free a texture the brush still paints (crash in SImage). */
	TArray<TStrongObjectPtr<class UTexture2D>> ThumbTextures;
	TSet<FString> ThumbPending;

	FString SessionId;
	FString SessionMapId;
	FString SearchFilter;
	FString StatusDetail;
	bool bUIVisible = true;
	bool bBusy = false;
	bool bLoadingMap = false;
	bool bListsBuilt = false;
	bool bLastBackendReady = false;
	/** last chrome state pushed to the top bar: did /health answer? */
	bool bLastBackendReachable = false;
	/** last chrome state pushed to the top bar: CARLA's connection is live */
	bool bLastCarlaConnected = false;
	bool bLastCarlaKnown = false;
	/** last endpoint printed in the chip, so a moved backend still repaints */
	bool bLastCarlaEndpointKnown = false;
	int32 LastCarlaPort = 0;
	/**
	 * The backend says a map load is in flight (preview_loading). Together with
	 * bLoadingMap it closes the window in which /health cannot be believed.
	 */
	bool bSessionLoadInFlight = false;
	/** value of IsCarlaProbeBlocked() the chip was last painted with */
	bool bProbeBlocked = false;
	/** seconds since the last GET /health poll (see TickRefresh) */
	double LastHealthPollAt = 0.0;
	/**
	 * Panel lifetime and backend-adoption bookkeeping. A backend that appears
	 * after Construct is adopted by re-issuing LoadCatalog() while /health
	 * answers, spaced out and never overlapping an in-flight attempt.
	 */
	double ConstructedAt = 0.0;
	double LastCatalogAttemptAt = 0.0;
	bool bLoggedBackendAnswer = false;
	/** explicit 启动后端 request in flight; the card shows a disabled busy state */
	bool bBackendStartPending = false;
	double BackendStartRequestedAt = 0.0;
	FString BackendStartError;
	bool bWarnStatus = false;
	FString QueuedMapId;
	bool IsDraggingView = false;
	bool bShowRoute = true;
	/** turntable frames rendered by tools/render_vehicle_turntable.py */
	static constexpr int32 SpinFrames = 16;
	double LoadStartedAt = 0.0;
};






