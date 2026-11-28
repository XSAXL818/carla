#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

/** One route preset of a map (as served by GET /maps). */
struct FMapPreset
{
	FString Id;
	FString Name;
	FString Kind;
	float LengthKm = 0.f;
	int32 Waypoints = 0;
};

struct FMapInfo
{
	FString Id;
	FString Name;
	int32 Roads = 0;
	int32 Junctions = 0;
	TArray<FMapPreset> Presets;
};

struct FVehicleInfo
{
	FString Id;
	FString Name;
	FString Cls;
};

struct FWeatherInfo
{
	FString Id;
	FString Name;
	FString Desc;
};

struct FTrafficInfo
{
	FString Id;
	FString Name;
	int32 Vehicles = 0;
	int32 Walkers = 0;
};

struct FAlgoOption
{
	FString Value;
	FString Label;
};

/**
 * One parameter of an algorithm, exactly as GET /algorithms describes it.
 *
 * Nothing here names a specific parameter: the panel dispatches on Type and
 * renders from Group / Min / Max / Step / Unit / Default, so a new algorithm or
 * a new parameter only needs the backend catalogue changed.
 */
struct FAlgoParam
{
	FString Key;
	FString Label;
	/** enum | number | bool. Any other value is rendered read-only (see
	 *  STestPlatformUI::UnknownParamRow) instead of being dropped or crashing. */
	FString Type;
	FString Unit;
	/** Schema grouping; empty means "no group heading". */
	FString Group;
	float Min = 0.f;
	float Max = 100.f;
	float Step = 1.f;
	/** True when the schema carried both min and max, i.e. a slider is possible. */
	bool bHasRange = false;
	float NumberDefault = 0.f;
	bool bBoolDefault = false;
	FString StringDefault;
	TArray<FAlgoOption> Options;
};

/**
 * One algorithm parameter's value as the panel holds it.
 *
 * Which field matters is decided by the schema's Type, never by the key, so the
 * panel can carry parameters it has never heard of.
 */
struct FAlgoParamValue
{
	/** The schema Type this value was created for, so a value cannot be read
	 *  back as the wrong kind after the schema changed underneath it. */
	FString Type;
	FString StringValue; // enum (and any string-shaped type)
	float Number = 0.f;
	bool bBool = false;
	/** True once the user moved the control. False means "still the schema
	 *  default", which is what makes switching algorithms pick up the new
	 *  schema's default instead of the old algorithm's value. */
	bool bUserSet = false;
};

/** The schema's default for one parameter. */
FAlgoParamValue DefaultAlgoParamValue(const FAlgoParam& Param);

/**
 * The value the config must carry for Param: the user's when they set one, the
 * schema default otherwise. The panel renders with this same call, so what the
 * user sees and what is posted cannot drift apart.
 */
FAlgoParamValue ResolveAlgoParamValue(const FAlgoParam& Param,
	const TMap<FString, FAlgoParamValue>& Values);

/** The schema's label, or a readable fallback derived from the key. */
FString AlgoParamLabel(const FAlgoParam& Param);

struct FAlgoInfo
{
	FString Id;
	FString Name;
	FString Desc;
	TArray<FAlgoParam> Params;
};

/** What the UI currently has selected. */
struct FPlatformSelection
{
	FString MapId;
	FString RoutePresetId;
	FString EgoBlueprint;
	FString WeatherId;
	FString TrafficId;
	FString AlgoId;
	/**
	 * Every algorithm parameter the selected algorithm's schema declared, keyed
	 * by the schema key. There is no per-parameter field: adding a parameter is a
	 * backend-only change (see FAlgoParam).
	 */
	TMap<FString, FAlgoParamValue> AlgoParams;
};

/** Live session state polled from the backend. */
struct FSessionStatus
{
	bool bValid = false;
	FString Id;
	FString State = TEXT("none");
	FString Error;
	FString MapId;
	int32 Tick = 0;
	int32 Events = 0;
	float SimTime = 0.f;
	float SpeedKmh = 0.f;
	/**
	 * The clock the backend says the session is really running at. The simulator
	 * can drop synchronous mode under a live session (the engine's
	 * carla.SyncCueTimeout, or another client applying settings), and then there
	 * is no fixed delta any more: the panel must stop printing one rather than
	 * keep the value it saw when the session started.
	 */
	bool bSyncMode = false;
	bool bSyncLost = false;
	float FixedDelta = 0.f;
	FString SyncLostDetail;

	bool IsRunning() const { return State == TEXT("running"); }
	bool IsPreviewing() const { return State == TEXT("preview") || State == TEXT("preview_loading"); }
	bool IsActive() const { return IsRunning() || IsPreviewing(); }
};

/**
 * Thin client for the Python backend: catalog data + session lifecycle.
 * All callbacks fire on the game thread (UE's HTTP module tick).
 */
class FTestPlatformApi : public TSharedFromThis<FTestPlatformApi>
{
public:
	void LoadCatalog();
	void SelectMap(const FString& MapId);

	/**
	 * GET /health and read carla.connected. The catalog endpoints answer with
	 * the simulator closed, so IsReady() only means "the backend answered" —
	 * this is the one signal that says whether CARLA's RPC server is actually
	 * up. Asynchronous; poll it rather than awaiting it.
	 *
	 * bForce asks the backend to re-probe instead of serving its 5 s cache: an
	 * answer produced while the simulator was blocked (a 30-90 s map load) says
	 * nothing about CARLA now, so the first poll after such a window forces it.
	 */
	void LoadHealth(bool bForce = false);
	/** What the last /health answer said about carla.connected. */
	bool IsCarlaConnected() const { return bCarlaConnected; }
	/** False until a /health answer has been parsed (backend down or starting). */
	bool HasCarlaState() const { return bCarlaKnown; }

	/**
	 * The CARLA RPC endpoint the backend says it is probing (/health ->
	 * carla.host / carla.port). The UI prints this instead of assuming 2000.
	 * False until an answer carrying both fields has been parsed.
	 */
	bool HasCarlaEndpoint() const { return bCarlaEndpointKnown; }
	const FString& CarlaHost() const { return CarlaProbeHost; }
	int32 CarlaPort() const { return CarlaProbePort; }

	/**
	 * Forget what /health last said about carla.connected (the endpoint stays
	 * known). Called while a map load blocks the simulator: a probe that runs
	 * then can only measure the block, so its "connected: false" must not be
	 * shown as 未连接. Answers already in flight when this is called are
	 * discarded instead of being applied afterwards.
	 */
	void InvalidateCarlaState();

	/**
	 * Liveness of the backend process itself, as last reported by GET /health.
	 * IsReady() only means "the catalog was loaded once" and stays true after
	 * the backend dies; this is the signal that follows the process.
	 */
	bool IsBackendResponding() const { return bBackendResponding; }
	/** False until a /health request has completed, so "not running" is not claimed before the first probe. */
	bool HasBackendAnswer() const { return bBackendAnswerKnown; }
	/** True while the five catalog requests of a LoadCatalog() call are still outstanding. */
	bool IsCatalogLoadInFlight() const { return PendingCatalogRequests > 0; }

	/**
	 * Bumped every time a /algorithms answer has been parsed into AlgoCache.
	 *
	 * The panel rebuilds its lists from this rather than from "the catalog was
	 * loaded once": ReloadCatalog() used to clear the built flag, which made the
	 * very next tick rebuild from the *old* cache (the new answer had not arrived
	 * yet), so a schema change on the backend never reached the panel without an
	 * editor restart. A revision the answers themselves advance cannot race that
	 * way.
	 */
	uint64 CatalogRevision() const { return AlgoCatalogRevision; }

	bool IsReady() const { return bReady; }
	const TArray<FMapInfo>& Maps() const { return MapsCache; }
	const TArray<FVehicleInfo>& Vehicles() const { return VehiclesCache; }
	const TArray<FWeatherInfo>& Weather() const { return WeatherCache; }
	const TArray<FTrafficInfo>& Traffic() const { return TrafficCache; }
	const TArray<FAlgoInfo>& Algorithms() const { return AlgoCache; }

	const FMapInfo* FindMap(const FString& MapId) const;
	const TArray<FMapPreset>& PresetsFor(const FString& MapId) const;
	const FAlgoInfo* SelectedAlgorithm(const FString& AlgoId) const;

	using FStatusCallback = TFunction<void(bool /*bOk*/, const FSessionStatus&, const FString& /*Error*/)>;
	using FSimpleCallback = TFunction<void(bool /*bOk*/, const FString& /*Error*/)>;

	/**
	 * The single resolved backend root URL. -TestPlatformBackend= overrides it;
	 * without the switch it is http://127.0.0.1:8765.
	 */
	static FString RootUrl();

	void CreateSession(const FPlatformSelection& Selection, FStatusCallback Done);
	/** One call: tear down the old session, load this map (live switch). */
	void PreviewMap(const FPlatformSelection& Selection, FStatusCallback Done);
	/** Raw bytes, used for map/vehicle card art (PNG). */
	void FetchBytes(const FString& Path, TFunction<void(bool, const TArray<uint8>&)> Done);
	/** Live control of the running preview: weather and route overlay. */
	void SetWeather(const FString& SessionId, const FString& Preset, FSimpleCallback Done);
	/**
	 * Replace the live session's algorithm + parameter block without reloading
	 * anything. The panel's controls can be changed after the map preview has
	 * been posted, and /start builds the agent from the config the backend
	 * holds -- so without this the parameters a user sees are not the ones that
	 * run. Same schema-driven payload as the config (see BuildAlgorithmJson).
	 */
	void SetAlgorithm(const FString& SessionId, const FPlatformSelection& Selection,
		FSimpleCallback Done);
	void SetRoute(const FString& SessionId, bool bVisible, bool bApplyVisible,
		const FString& PresetId, FSimpleCallback Done);
	void PreviewSession(const FString& SessionId, FStatusCallback Done);
	void StartSession(const FString& SessionId, FStatusCallback Done);
	void StopSession(const FString& SessionId, FStatusCallback Done);
	void FetchCurrent(FStatusCallback Done);

	FString LastError;

private:
	void GetJson(const FString& Path, TFunction<void(bool, const TSharedPtr<class FJsonObject>&)> Done);
	void PostJson(const FString& Path, const FString& Body,
		TFunction<void(bool, const TSharedPtr<FJsonObject>&, const FString&)> Done);
	FString BuildConfigJson(const FPlatformSelection& Selection) const;
	/** {"id":..., "params":{...}} straight from the selected algorithm's schema. */
	FString BuildAlgorithmJson(const FPlatformSelection& Selection) const;
	static FSessionStatus ParseStatus(const TSharedPtr<class FJsonObject>& Root);

	TArray<FMapInfo> MapsCache;
	TArray<FVehicleInfo> VehiclesCache;
	TArray<FWeatherInfo> WeatherCache;
	TArray<FTrafficInfo> TrafficCache;
	TArray<FAlgoInfo> AlgoCache;
	/** Advanced by every parsed /algorithms answer; see CatalogRevision(). */
	uint64 AlgoCatalogRevision = 0;
	TMap<FString, TArray<FMapPreset>> PresetsByMap;
	bool bReady = false;
	/** outcome of the last GET /health: the backend process answered (2xx JSON) */
	bool bBackendResponding = false;
	bool bBackendAnswerKnown = false;
	/** outstanding requests of LoadCatalog(), so the poller cannot pile attempts up */
	int32 PendingCatalogRequests = 0;
	bool bCarlaConnected = false;
	bool bCarlaKnown = false;
	/** carla.host / carla.port from the last /health answer that carried them */
	bool bCarlaEndpointKnown = false;
	FString CarlaProbeHost;
	int32 CarlaProbePort = 0;
	/**
	 * Bumped by InvalidateCarlaState(). A /health answer captures it when the
	 * request is issued and is dropped on arrival if it no longer matches, so a
	 * probe from before the invalidation cannot resurrect a stale verdict.
	 */
	uint64 HealthEpoch = 0;
};



