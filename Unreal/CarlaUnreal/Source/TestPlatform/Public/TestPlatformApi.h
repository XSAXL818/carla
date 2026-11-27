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

struct FAlgoParam
{
	FString Key;
	FString Label;
	FString Type;      // enum | number | bool
	FString Unit;
	FString Group;
	float Min = 0.f;
	float Max = 100.f;
	float Step = 1.f;
	float NumberDefault = 0.f;
	bool bBoolDefault = false;
	FString StringDefault;
	TArray<FAlgoOption> Options;
};

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
	FString Behavior = TEXT("normal");
	float TargetSpeedKmh = 50.f;
	bool bIgnoreTrafficLights = false;
	bool bIgnoreStopSigns = false;
	bool bIgnoreVehicles = false;
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
	static FSessionStatus ParseStatus(const TSharedPtr<class FJsonObject>& Root);

	TArray<FMapInfo> MapsCache;
	TArray<FVehicleInfo> VehiclesCache;
	TArray<FWeatherInfo> WeatherCache;
	TArray<FTrafficInfo> TrafficCache;
	TArray<FAlgoInfo> AlgoCache;
	TMap<FString, TArray<FMapPreset>> PresetsByMap;
	bool bReady = false;
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



