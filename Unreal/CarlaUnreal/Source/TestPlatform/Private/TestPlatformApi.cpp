#include "TestPlatformApi.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	double NumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Fallback = 0.0)
	{
		double Value = Fallback;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(Key, Value);
		}
		return Value;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Key, Value);
		}
		return Value;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj.IsValid())
		{
			Obj->TryGetArrayField(Key, Array);
		}
		return Array;
	}

	TSharedPtr<FJsonObject> AsObject(const TSharedPtr<FJsonValue>& Value)
	{
		return (Value.IsValid() && Value->Type == EJson::Object) ? Value->AsObject() : nullptr;
	}
}

FString FTestPlatformApi::RootUrl()
{
	FString Override;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TestPlatformBackend="), Override))
	{
		return TEXT("http://127.0.0.1:8765");
	}

	// Every HTTP caller appends "/..." to this base, so a trailing slash would
	// produce "//sessions/current" (a 404), and a hand-typed host:port carries
	// no scheme at all. Normalise here, the one place the URL is resolved.
	Override.TrimStartAndEndInline();
	if (!Override.Contains(TEXT("://")))
	{
		Override = FString(TEXT("http://")) + Override;
	}
	while (Override.EndsWith(TEXT("/")))
	{
		Override.LeftChopInline(1);
	}
	if (Override.IsEmpty() || Override.EndsWith(TEXT(":")))
	{
		// Nothing usable was supplied (e.g. "http://"); keep the default base.
		return TEXT("http://127.0.0.1:8765");
	}
	return Override;
}

// ---------------------------------------------------------------------- //
// HTTP plumbing
// ---------------------------------------------------------------------- //
void FTestPlatformApi::GetJson(
	const FString& Path, TFunction<void(bool, const TSharedPtr<FJsonObject>&)> Done)
{
	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(RootUrl() + Path);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(12.f);
	Request->OnProcessRequestComplete().BindLambda(
		[Done](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			TSharedPtr<FJsonObject> Root;
			const bool bHttpOk = bOk && Response.IsValid() &&
				Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300;
			if (bHttpOk)
			{
				const TSharedRef<TJsonReader<>> Reader =
					TJsonReaderFactory<>::Create(Response->GetContentAsString());
				FJsonSerializer::Deserialize(Reader, Root);
			}
			if (Done)
			{
				Done(bHttpOk && Root.IsValid(), Root);
			}
		});
	Request->ProcessRequest();
}

void FTestPlatformApi::PostJson(
	const FString& Path, const FString& Body,
	TFunction<void(bool, const TSharedPtr<FJsonObject>&, const FString&)> Done)
{
	const FString Endpoint = RootUrl();
	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint + Path);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);
	Request->SetTimeout(180.f); // preview may load a whole map
	Request->OnProcessRequestComplete().BindLambda(
		[Done, Endpoint](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			TSharedPtr<FJsonObject> Root;
			FString Error;
			bool bHttpOk = bOk && Response.IsValid();
			if (bHttpOk)
			{
				const int32 Code = Response->GetResponseCode();
				bHttpOk = Code >= 200 && Code < 300;
				const TSharedRef<TJsonReader<>> Reader =
					TJsonReaderFactory<>::Create(Response->GetContentAsString());
				FJsonSerializer::Deserialize(Reader, Root);
				if (!bHttpOk && Root.IsValid())
				{
					Error = StringField(Root, TEXT("detail"));
				}
				if (!bHttpOk && Error.IsEmpty() && Response->GetResponseCode() != 0)
				{
					Error = FString::Printf(TEXT("HTTP %d"), Code);
				}
			}
			else
			{
				Error = FString::Printf(TEXT("后端无响应（%s）"), *Endpoint);
			}
			if (Done)
			{
				Done(bHttpOk, Root, Error);
			}
		});
	Request->ProcessRequest();
}

// ---------------------------------------------------------------------- //
// catalog
// ---------------------------------------------------------------------- //
void FTestPlatformApi::LoadCatalog()
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();

	GetJson(TEXT("/maps"), [Weak](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->MapsCache.Reset();
		if (const TArray<TSharedPtr<FJsonValue>>* Maps = ArrayField(Root, TEXT("maps")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Maps)
			{
				const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
				if (!Obj.IsValid())
				{
					continue;
				}
				FMapInfo Info;
				Info.Id = StringField(Obj, TEXT("id"));
				Info.Name = StringField(Obj, TEXT("name"));
				if (Info.Name.IsEmpty())
				{
					Info.Name = Info.Id;
				}
				Info.Roads = static_cast<int32>(NumberField(Obj, TEXT("roads")));
				Info.Junctions = static_cast<int32>(NumberField(Obj, TEXT("junctions")));
				if (const TArray<TSharedPtr<FJsonValue>>* Presets = ArrayField(Obj, TEXT("presets")))
				{
					for (const TSharedPtr<FJsonValue>& PresetValue : *Presets)
					{
						const TSharedPtr<FJsonObject> PresetObj = AsObject(PresetValue);
						if (!PresetObj.IsValid())
						{
							continue;
						}
						FMapPreset Preset;
						Preset.Id = StringField(PresetObj, TEXT("id"));
						Preset.Name = StringField(PresetObj, TEXT("name"));
						Preset.Kind = StringField(PresetObj, TEXT("kind"));
						Preset.LengthKm = static_cast<float>(NumberField(PresetObj, TEXT("length_km")));
						Preset.Waypoints = static_cast<int32>(NumberField(PresetObj, TEXT("waypoints")));
						Info.Presets.Add(Preset);
					}
				}
				This->MapsCache.Add(Info);
				This->PresetsByMap.Add(Info.Id, Info.Presets);
			}
		}
		This->LastError = bOk ? FString() : TEXT("读取地图列表失败");
		This->bReady = This->MapsCache.Num() > 0;
	});

	GetJson(TEXT("/vehicles"), [Weak](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->VehiclesCache.Reset();
		if (const TArray<TSharedPtr<FJsonValue>>* Items = ArrayField(Root, TEXT("vehicles")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Items)
			{
				const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
				if (!Obj.IsValid())
				{
					continue;
				}
				FVehicleInfo Info;
				Info.Id = StringField(Obj, TEXT("id"));
				Info.Name = StringField(Obj, TEXT("name"));
				Info.Cls = StringField(Obj, TEXT("cls"));
				This->VehiclesCache.Add(Info);
			}
		}
	});

	GetJson(TEXT("/weather"), [Weak](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->WeatherCache.Reset();
		if (const TArray<TSharedPtr<FJsonValue>>* Items = ArrayField(Root, TEXT("presets")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Items)
			{
				const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
				if (!Obj.IsValid())
				{
					continue;
				}
				FWeatherInfo Info;
				Info.Id = StringField(Obj, TEXT("id"));
				Info.Name = StringField(Obj, TEXT("name"));
				Info.Desc = StringField(Obj, TEXT("desc"));
				This->WeatherCache.Add(Info);
			}
		}
	});

	GetJson(TEXT("/traffic"), [Weak](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->TrafficCache.Reset();
		if (const TArray<TSharedPtr<FJsonValue>>* Items = ArrayField(Root, TEXT("presets")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Items)
			{
				const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
				if (!Obj.IsValid())
				{
					continue;
				}
				FTrafficInfo Info;
				Info.Id = StringField(Obj, TEXT("id"));
				Info.Name = StringField(Obj, TEXT("name"));
				Info.Vehicles = static_cast<int32>(NumberField(Obj, TEXT("vehicles")));
				Info.Walkers = static_cast<int32>(NumberField(Obj, TEXT("walkers")));
				This->TrafficCache.Add(Info);
			}
		}
	});

	GetJson(TEXT("/algorithms"), [Weak](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->AlgoCache.Reset();
		if (const TArray<TSharedPtr<FJsonValue>>* Items = ArrayField(Root, TEXT("algorithms")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Items)
			{
				const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
				if (!Obj.IsValid())
				{
					continue;
				}
				FAlgoInfo Info;
				Info.Id = StringField(Obj, TEXT("id"));
				Info.Name = StringField(Obj, TEXT("name"));
				Info.Desc = StringField(Obj, TEXT("desc"));
				if (const TArray<TSharedPtr<FJsonValue>>* Params = ArrayField(Obj, TEXT("params")))
				{
					for (const TSharedPtr<FJsonValue>& ParamValue : *Params)
					{
						const TSharedPtr<FJsonObject> ParamObj = AsObject(ParamValue);
						if (!ParamObj.IsValid())
						{
							continue;
						}
						FAlgoParam Param;
						Param.Key = StringField(ParamObj, TEXT("key"));
						Param.Label = StringField(ParamObj, TEXT("label"));
						Param.Type = StringField(ParamObj, TEXT("type"));
						Param.Unit = StringField(ParamObj, TEXT("unit"));
						Param.Group = StringField(ParamObj, TEXT("group"));
						Param.Min = static_cast<float>(NumberField(ParamObj, TEXT("min")));
						Param.Max = static_cast<float>(NumberField(ParamObj, TEXT("max"), 100.0));
						Param.Step = static_cast<float>(NumberField(ParamObj, TEXT("step"), 1.0));
						if (Param.Type == TEXT("number"))
						{
							Param.NumberDefault = static_cast<float>(NumberField(ParamObj, TEXT("default")));
						}
						else if (Param.Type == TEXT("bool"))
						{
							bool bValue = false;
							if (ParamObj->TryGetBoolField(TEXT("default"), bValue))
							{
								Param.bBoolDefault = bValue;
							}
						}
						else
						{
							Param.StringDefault = StringField(ParamObj, TEXT("default"));
							if (const TArray<TSharedPtr<FJsonValue>>* Options =
									ArrayField(ParamObj, TEXT("options")))
							{
								for (const TSharedPtr<FJsonValue>& OptionValue : *Options)
								{
									const TSharedPtr<FJsonObject> OptionObj = AsObject(OptionValue);
									if (!OptionObj.IsValid())
									{
										continue;
									}
									FAlgoOption Option;
									Option.Value = StringField(OptionObj, TEXT("value"));
									Option.Label = StringField(OptionObj, TEXT("label"));
									Param.Options.Add(Option);
								}
							}
						}
						Info.Params.Add(Param);
					}
				}
				This->AlgoCache.Add(Info);
			}
		}
	});
}

// ---------------------------------------------------------------------- //
// health: is CARLA actually up?
// ---------------------------------------------------------------------- //
void FTestPlatformApi::LoadHealth(bool bForce)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	// A request issued before InvalidateCarlaState() belongs to a window in which
	// the probe could not tell anything (blocked simulator); drop it on arrival.
	const uint64 Epoch = HealthEpoch;

	GetJson(bForce ? TEXT("/health?force=true") : TEXT("/health"),
		[Weak, Epoch](bool bOk, const TSharedPtr<FJsonObject>& Root)
	{
		TSharedPtr<FTestPlatformApi> This = Weak.Pin();
		if (!This.IsValid())
		{
			return;
		}
		if (Epoch != This->HealthEpoch)
		{
			return;
		}
		// /health carries {"carla": {"connected": bool, "host": ..., "port": ...}}.
		// Without that block the state stays unknown: a backend that answers with
		// the simulator closed must not be reported as a live CARLA connection.
		bool bConnected = false;
		const TSharedPtr<FJsonObject>* Carla = nullptr;
		if (bOk && Root.IsValid())
		{
			Root->TryGetObjectField(TEXT("carla"), Carla);
		}
		This->bCarlaKnown = Carla && (*Carla)->TryGetBoolField(TEXT("connected"), bConnected);
		This->bCarlaConnected = This->bCarlaKnown && bConnected;

		// The same block names the endpoint the backend really probes
		// (CARLA_HOST / CARLA_PORT). The top bar prints this, not a constant:
		// the state is measured there, so the address must be the same one.
		if (Carla && Carla->IsValid())
		{
			FString Host;
			double Port = 0.0;
			if ((*Carla)->TryGetStringField(TEXT("host"), Host) && !Host.IsEmpty() &&
				(*Carla)->TryGetNumberField(TEXT("port"), Port) && Port > 0.0 &&
				Port <= 65535.0)
			{
				This->CarlaProbeHost = Host;
				This->CarlaProbePort = static_cast<int32>(Port);
				This->bCarlaEndpointKnown = true;
			}
		}
	});
}

void FTestPlatformApi::InvalidateCarlaState()
{
	// Bumping the epoch also drops answers already in flight (see LoadHealth).
	++HealthEpoch;
	bCarlaKnown = false;
	bCarlaConnected = false;
}

void FTestPlatformApi::SelectMap(const FString& MapId)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	GetJson(FString::Printf(TEXT("/maps/%s/routes"), *MapId),
		[Weak, MapId](bool bOk, const TSharedPtr<FJsonObject>& Root)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (!This.IsValid() || !Root.IsValid())
			{
				return;
			}
			TArray<FMapPreset> Presets;
			if (const TArray<TSharedPtr<FJsonValue>>* Items = ArrayField(Root, TEXT("presets")))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Items)
				{
					const TSharedPtr<FJsonObject> Obj = AsObject(Entry);
					if (!Obj.IsValid())
					{
						continue;
					}
					FMapPreset Preset;
					Preset.Id = StringField(Obj, TEXT("id"));
					Preset.Name = StringField(Obj, TEXT("name"));
					Preset.Kind = StringField(Obj, TEXT("kind"));
					Preset.LengthKm = static_cast<float>(NumberField(Obj, TEXT("length_km")));
					Preset.Waypoints = static_cast<int32>(NumberField(Obj, TEXT("waypoints")));
					Presets.Add(Preset);
				}
			}
			if (Presets.Num() > 0)
			{
				This->PresetsByMap.Add(MapId, Presets);
			}
		});
}

const FMapInfo* FTestPlatformApi::FindMap(const FString& MapId) const
{
	return MapsCache.FindByPredicate([&MapId](const FMapInfo& Info) { return Info.Id == MapId; });
}

const TArray<FMapPreset>& FTestPlatformApi::PresetsFor(const FString& MapId) const
{
	if (const TArray<FMapPreset>* Presets = PresetsByMap.Find(MapId))
	{
		return *Presets;
	}
	static const TArray<FMapPreset> Empty;
	return Empty;
}

const FAlgoInfo* FTestPlatformApi::SelectedAlgorithm(const FString& AlgoId) const
{
	return AlgoCache.FindByPredicate([&AlgoId](const FAlgoInfo& Info) { return Info.Id == AlgoId; });
}

// ---------------------------------------------------------------------- //
// session lifecycle
// ---------------------------------------------------------------------- //
FString FTestPlatformApi::BuildConfigJson(const FPlatformSelection& Selection) const
{
	const FMapInfo* Map = FindMap(Selection.MapId);
	FString RouteJson = TEXT("{\"source\":\"preset\",\"preset_id\":\"loop\"}");
	if (!Selection.RoutePresetId.IsEmpty())
	{
		RouteJson = FString::Printf(
			TEXT("{\"source\":\"preset\",\"preset_id\":\"%s\"}"), *Selection.RoutePresetId);
	}
	else if (Map && Map->Presets.Num() > 0)
	{
		RouteJson = FString::Printf(
			TEXT("{\"source\":\"preset\",\"preset_id\":\"%s\"}"), *Map->Presets[0].Id);
	}

	const FString Ego = Selection.EgoBlueprint.IsEmpty()
		? TEXT("vehicle.tesla.model3") : Selection.EgoBlueprint;
	const FString Weather = Selection.WeatherId.IsEmpty()
		? TEXT("ClearNoon") : Selection.WeatherId;
	const FString Traffic = Selection.TrafficId.IsEmpty()
		? TEXT("light") : Selection.TrafficId;
	const FString Algo = Selection.AlgoId.IsEmpty()
		? TEXT("carla.behavior") : Selection.AlgoId;

	return FString::Printf(
		TEXT("{\"schema_version\":1,\"map\":\"%s\",")
		TEXT("\"ego\":{\"blueprint\":\"%s\",\"spawn\":{\"mode\":\"route_start\"}},")
		TEXT("\"route\":%s,")
		TEXT("\"weather\":{\"preset\":\"%s\"},")
		TEXT("\"traffic\":{\"preset\":\"%s\",\"autopilot\":true,\"seed\":20261126},")
		TEXT("\"algorithm\":{\"id\":\"%s\",\"params\":{\"behavior\":\"%s\",\"target_speed_kmh\":%.0f,")
		TEXT("\"ignore_traffic_lights\":%s,\"ignore_stop_signs\":%s,\"ignore_vehicles\":%s}},")
		TEXT("\"run\":{\"sync_mode\":true,\"fixed_delta_seconds\":0.05,\"record\":true,")
		TEXT("\"export\":{\"metrics_csv\":true,\"events_json\":true,\"config_json\":true},")
		TEXT("\"max_duration_s\":300}}"),
		*Selection.MapId,
		*Ego,
		*RouteJson,
		*Weather,
		*Traffic,
		*Algo,
		*Selection.Behavior,
		Selection.TargetSpeedKmh,
		Selection.bIgnoreTrafficLights ? TEXT("true") : TEXT("false"),
		Selection.bIgnoreStopSigns ? TEXT("true") : TEXT("false"),
		Selection.bIgnoreVehicles ? TEXT("true") : TEXT("false"));
}

FSessionStatus FTestPlatformApi::ParseStatus(const TSharedPtr<FJsonObject>& Root)
{
	FSessionStatus Status;
	if (!Root.IsValid())
	{
		return Status;
	}
	Status.bValid = true;
	Status.Id = StringField(Root, TEXT("id"));
	Status.State = StringField(Root, TEXT("state"));
	if (Status.State.IsEmpty())
	{
		Status.State = TEXT("none");
	}
	Status.Error = StringField(Root, TEXT("error"));
	Status.MapId = StringField(Root, TEXT("map"));
	Status.Tick = static_cast<int32>(NumberField(Root, TEXT("tick")));
	Status.Events = static_cast<int32>(NumberField(Root, TEXT("events")));
	Status.SimTime = static_cast<float>(NumberField(Root, TEXT("sim_time")));

	const TSharedPtr<FJsonObject>* Ego = nullptr;
	if (Root->TryGetObjectField(TEXT("ego"), Ego) && Ego && Ego->IsValid())
	{
		Status.SpeedKmh = static_cast<float>(NumberField(*Ego, TEXT("speed_kmh")));
	}
	return Status;
}

void FTestPlatformApi::CreateSession(const FPlatformSelection& Selection, FStatusCallback Done)
{
	const FString Body = BuildConfigJson(Selection);
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	PostJson(TEXT("/sessions"), Body,
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root, const FString& Error)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (This.IsValid() && !bOk)
			{
				This->LastError = Error;
			}
			if (Done)
			{
				Done(bOk, ParseStatus(Root), Error);
			}
		});
}

void FTestPlatformApi::PreviewSession(const FString& SessionId, FStatusCallback Done)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	PostJson(FString::Printf(TEXT("/sessions/%s/preview"), *SessionId), FString(),
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root, const FString& Error)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (This.IsValid() && !bOk)
			{
				This->LastError = Error;
			}
			if (Done)
			{
				Done(bOk, ParseStatus(Root), Error);
			}
		});
}

void FTestPlatformApi::StartSession(const FString& SessionId, FStatusCallback Done)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	PostJson(FString::Printf(TEXT("/sessions/%s/start"), *SessionId), FString(),
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root, const FString& Error)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (This.IsValid() && !bOk)
			{
				This->LastError = Error;
			}
			if (Done)
			{
				Done(bOk, ParseStatus(Root), Error);
			}
		});
}

void FTestPlatformApi::StopSession(const FString& SessionId, FStatusCallback Done)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	PostJson(FString::Printf(TEXT("/sessions/%s/stop"), *SessionId), FString(),
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root, const FString& Error)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (This.IsValid() && !bOk)
			{
				This->LastError = Error;
			}
			if (Done)
			{
				Done(bOk, ParseStatus(Root), Error);
			}
		});
}

void FTestPlatformApi::FetchCurrent(FStatusCallback Done)
{
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	GetJson(TEXT("/sessions/current"),
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root)
		{
			if (Done)
			{
				Done(bOk, ParseStatus(Root), FString());
			}
		});
}


void FTestPlatformApi::PreviewMap(const FPlatformSelection& Selection, FStatusCallback Done)
{
	const FString Body = BuildConfigJson(Selection);
	TWeakPtr<FTestPlatformApi> Weak = AsShared();
	PostJson(TEXT("/sessions/preview_map"), Body,
		[Weak, Done](bool bOk, const TSharedPtr<FJsonObject>& Root, const FString& Error)
		{
			TSharedPtr<FTestPlatformApi> This = Weak.Pin();
			if (This.IsValid() && !bOk)
			{
				This->LastError = Error;
			}
			if (Done)
			{
				Done(bOk, ParseStatus(Root), Error);
			}
		});
}

void FTestPlatformApi::FetchBytes(const FString& Path,
	TFunction<void(bool, const TArray<uint8>&)> Done)
{
	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(RootUrl() + Path);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(20.f);
	Request->OnProcessRequestComplete().BindLambda(
		[Done](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			TArray<uint8> Bytes;
			const bool bHttpOk = bOk && Response.IsValid() &&
				Response->GetResponseCode() >= 200 && Response->GetResponseCode() < 300;
			if (bHttpOk)
			{
				Bytes = Response->GetContent();
			}
			if (Done)
			{
				Done(bHttpOk, Bytes);
			}
		});
	Request->ProcessRequest();
}

void FTestPlatformApi::SetWeather(const FString& SessionId, const FString& Preset,
	FSimpleCallback Done)
{
	const FString Body = FString::Printf(TEXT("{\"preset\":\"%s\"}"), *Preset);
	PostJson(FString::Printf(TEXT("/sessions/%s/weather"), *SessionId), Body,
		[Done](bool bOk, const TSharedPtr<FJsonObject>&, const FString& Error)
		{
			if (Done)
			{
				Done(bOk, Error);
			}
		});
}

void FTestPlatformApi::SetRoute(const FString& SessionId, bool bVisible,
	bool bApplyVisible, const FString& PresetId, FSimpleCallback Done)
{
	FString Body = TEXT("{");
	if (!PresetId.IsEmpty())
	{
		Body += FString::Printf(TEXT("\"preset_id\":\"%s\""), *PresetId);
	}
	if (bApplyVisible)
	{
		if (Body.Len() > 1)
		{
			Body += TEXT(",");
		}
		Body += FString::Printf(TEXT("\"visible\":%s"),
			bVisible ? TEXT("true") : TEXT("false"));
	}
	Body += TEXT("}");
	PostJson(FString::Printf(TEXT("/sessions/%s/route"), *SessionId), Body,
		[Done](bool bOk, const TSharedPtr<FJsonObject>&, const FString& Error)
		{
			if (Done)
			{
				Done(bOk, Error);
			}
		});
}
