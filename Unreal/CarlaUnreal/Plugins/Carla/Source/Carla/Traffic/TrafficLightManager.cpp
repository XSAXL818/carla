// Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "TrafficLightManager.h"
#include "GeoTrafficSign.h"
#include "Game/CarlaStatics.h"
#include "Game/CarlaGameModeBase.h"
#include "StopSignComponent.h"
#include "YieldSignComponent.h"
#include "SpeedLimitComponent.h"
#include "TrafficSignHeightUtils.h"
#include "Components/BoxComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TrafficLightBase.h"
#include "OpenDrive/OpenDrive.h"
#include "OpenDrive/MapLogicParser.h"
#include "UObject/UObjectGlobals.h"

#include <util/disable-ue4-macros.h>
#include <carla/rpc/String.h>
#include <carla/road/SignalType.h>
#include <carla/opendrive/OpenDriveParser.h>
#include <util/enable-ue4-macros.h>


#include <string>

static TAutoConsoleVariable<bool> CVarCarlaHideSpawnedSignsWhenPlaced(
    TEXT("carla.TrafficSigns.HideSpawnedWhenPlaced"),
    true,
    TEXT("On World Partition maps, hide the signs spawned from the OpenDRIVE once a sign ")
    TEXT("placed in the level streams in. Their traffic logic stays. Read at level load."),
    ECVF_Default);

namespace
{
  TSubclassOf<AActor> LoadDefaultActorClass(const TCHAR *Path)
  {
    UClass *LoadedClass = LoadClass<AActor>(nullptr, Path);
    if (!LoadedClass)
    {
      UE_LOG(LogCarla, Warning, TEXT("Could not load default actor class '%s'"), Path);
    }
    return LoadedClass;
  }
}

ATrafficLightManager::ATrafficLightManager()
{
  PrimaryActorTick.bCanEverTick = true;
  PrimaryActorTick.bStartWithTickEnabled = false;
  SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
  RootComponent = SceneComponent;
  TrafficLightGroupMissingId = -2;
}

void ATrafficLightManager::EnsureDefaultModelsLoaded()
{
  if (!TrafficLightModel_RHT)
  {
    TrafficLightModel_RHT = LoadDefaultActorClass(
        TEXT("/Game/Carla/Blueprints/TrafficLight/BP_TLOpenDrive_RHT.BP_TLOpenDrive_RHT_C"));
  }
  if (!TrafficLightModel_LHT)
  {
    TrafficLightModel_LHT = LoadDefaultActorClass(
        TEXT("/Game/Carla/Blueprints/TrafficLight/BP_TLOpenDrive_LHT.BP_TLOpenDrive_LHT_C"));
  }

  const FString StopSignType = carla::road::SignalType::StopSign().c_str();
  if (!TrafficSignsModels.FindRef(StopSignType))
  {
    if (TSubclassOf<AActor> Model = LoadDefaultActorClass(
            TEXT("/Game/Carla/Static/TrafficSign/BP_Stop01.BP_Stop01_C")))
    {
      TrafficSignsModels.Add(StopSignType, Model);
    }
  }
  if (!SignComponentModels.FindRef(StopSignType))
  {
    SignComponentModels.Add(StopSignType, UStopSignComponent::StaticClass());
  }

  const FString YieldSignType = carla::road::SignalType::YieldSign().c_str();
  if (!TrafficSignsModels.FindRef(YieldSignType))
  {
    if (TSubclassOf<AActor> Model = LoadDefaultActorClass(
            TEXT("/Game/Carla/Static/TrafficSign/BP_Yield01.BP_Yield01_C")))
    {
      TrafficSignsModels.Add(YieldSignType, Model);
    }
  }
  if (!SignComponentModels.FindRef(YieldSignType))
  {
    SignComponentModels.Add(YieldSignType, UYieldSignComponent::StaticClass());
  }

  auto EnsureSpeedLimit = [this](const TCHAR *Key, const TCHAR *Path)
  {
    const FString SpeedKey(Key);
    if (!SpeedLimitModels.FindRef(SpeedKey))
    {
      if (TSubclassOf<AActor> Model = LoadDefaultActorClass(Path))
      {
        SpeedLimitModels.Add(SpeedKey, Model);
      }
    }
  };
  EnsureSpeedLimit(TEXT("30"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit30.BP_SpeedLimit30_C"));
  EnsureSpeedLimit(TEXT("40"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit40.BP_SpeedLimit40_C"));
  EnsureSpeedLimit(TEXT("50"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit50.BP_SpeedLimit50_C"));
  EnsureSpeedLimit(TEXT("60"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit60.BP_SpeedLimit60_C"));
  EnsureSpeedLimit(TEXT("70"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit70.BP_SpeedLimit70_C"));
  EnsureSpeedLimit(TEXT("80"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit80.BP_SpeedLimit80_C"));
  EnsureSpeedLimit(TEXT("90"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit90.BP_SpeedLimit90_C"));
  EnsureSpeedLimit(TEXT("100"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit100.BP_SpeedLimit100_C"));
  EnsureSpeedLimit(TEXT("110"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit110.BP_SpeedLimit110_C"));
  EnsureSpeedLimit(TEXT("120"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit120.BP_SpeedLimit120_C"));

  struct FUSPlate { const TCHAR *Key; const TCHAR *Path; };
  const FUSPlate USPlates[] = {
      {TEXT("20"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit20_15.BP_SpeedLimit20_15_C")},
      {TEXT("25"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit25_15.BP_SpeedLimit25_15_C")},
      {TEXT("30"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit30_15.BP_SpeedLimit30_15_C")},
      {TEXT("50"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit50_40.BP_SpeedLimit50_40_C")},
      {TEXT("55"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit55_40.BP_SpeedLimit55_40_C")},
      {TEXT("75"), TEXT("/Game/Carla/Static/TrafficSign/BP_SpeedLimit75_45.BP_SpeedLimit75_45_C")},
  };
  for (const FUSPlate &Plate : USPlates)
  {
    const FString SpeedKey(Plate.Key);
    if (!SpeedLimitModels_US.FindRef(SpeedKey))
    {
      if (TSubclassOf<AActor> Model = LoadDefaultActorClass(Plate.Path))
      {
        SpeedLimitModels_US.Add(SpeedKey, Model);
      }
    }
  }
}

void ATrafficLightManager::RegisterLightComponentFromOpenDRIVE(UTrafficLightComponent * TrafficLightComponent)
{
  ACarlaGameModeBase *GM = UCarlaStatics::GetGameMode(GetWorld());
  check(GM);

  // Cast to std::string
  carla::road::SignId SignId(TCHAR_TO_UTF8(*(TrafficLightComponent->GetSignId())));

  ATrafficLightGroup* TrafficLightGroup;
  UTrafficLightController* TrafficLightController;

  const auto &Signal = GetMap()->GetSignals().at(SignId);
  if(Signal->GetControllers().size())
  {
    // Only one controller per signal
    auto ControllerId = *(Signal->GetControllers().begin());

    // Get controller
    const auto &Controller = GetMap()->GetControllers().at(ControllerId);
    if(Controller->GetJunctions().empty())
    {
      UE_LOG(LogCarla, Error, TEXT("No junctions in controller %d"), *(ControllerId.c_str()) );
      return;
    }
    // Get junction of the controller
    auto JunctionId = *(Controller->GetJunctions().begin());

    // Search/create TrafficGroup (junction traffic light manager)
    if(!TrafficGroups.Contains(JunctionId))
    {
      FActorSpawnParameters SpawnParams;
      SpawnParams.OverrideLevel = GM->GetULevelFromName("TrafficLights");
      auto * NewTrafficLightGroup =
          GetWorld()->SpawnActor<ATrafficLightGroup>(SpawnParams);
      NewTrafficLightGroup->JunctionId = JunctionId;
      TrafficGroups.Add(JunctionId, NewTrafficLightGroup);
    }
    TrafficLightGroup = TrafficGroups[JunctionId];

    // Search/create controller in the junction
    if(!TrafficControllers.Contains(ControllerId.c_str()))
    {
      auto *NewTrafficLightController = NewObject<UTrafficLightController>();
      NewTrafficLightController->SetControllerId(ControllerId.c_str());
      TrafficLightGroup->AddController(NewTrafficLightController);
      TrafficControllers.Add(ControllerId.c_str(), NewTrafficLightController);
    }
    TrafficLightController = TrafficControllers[ControllerId.c_str()];
  }
  else
  {
    FActorSpawnParameters SpawnParams;
    SpawnParams.OverrideLevel = GM->GetULevelFromName("TrafficLights");
    auto * NewTrafficLightGroup =
          GetWorld()->SpawnActor<ATrafficLightGroup>(SpawnParams);
    NewTrafficLightGroup->JunctionId = TrafficLightGroupMissingId;
    TrafficGroups.Add(NewTrafficLightGroup->JunctionId, NewTrafficLightGroup);
    TrafficLightGroup = NewTrafficLightGroup;

    auto *NewTrafficLightController = NewObject<UTrafficLightController>();
    NewTrafficLightController->SetControllerId(FString::FromInt(TrafficLightControllerMissingId));
    NewTrafficLightController->SetRedTime(10);
    TrafficLightGroup->AddController(NewTrafficLightController);
    TrafficControllers.Add(NewTrafficLightController->GetControllerId(), NewTrafficLightController);
    TrafficLightController = NewTrafficLightController;

    --TrafficLightGroupMissingId;
    --TrafficLightControllerMissingId;
  }

  TrafficLightController->AddTrafficLight(TrafficLightComponent);
  TrafficLightController->ResetState();
  TrafficSignComponents.Add(TrafficLightComponent->GetSignId(), TrafficLightComponent);

  TrafficLightGroup->ResetGroup();

}

void ATrafficLightManager::RegisterLightComponentGenerated(UTrafficLightComponent * TrafficLight)
{
  auto* Controller = TrafficLight->GetController();
  auto* Group = TrafficLight->GetGroup();
  if (!Controller || !Group)
  {
    UE_LOG(LogCarla, Error, TEXT("Missing group or controller for traffic light"));
    return;
  }
  if (TrafficLight->GetSignId() == "")
  {
    TrafficLight->SetSignId(FString::FromInt(TrafficLightComponentMissingId));
    --TrafficLightComponentMissingId;
  }
  if (Controller->GetControllerId() == "")
  {
    Controller->SetControllerId(FString::FromInt(TrafficLightControllerMissingId));
    --TrafficLightControllerMissingId;
  }
  if (Group->GetJunctionId() == -1)
  {
    Group->JunctionId = TrafficLightGroupMissingId;
    --TrafficLightGroupMissingId;
  }

  if (!TrafficControllers.Contains(Controller->GetControllerId()))
  {
    TrafficControllers.Add(Controller->GetControllerId(), Controller);
  }
  if (!TrafficGroups.Contains(Group->GetJunctionId()))
  {
    TrafficGroups.Add(Group->GetJunctionId(), Group);
  }
  if (!TrafficSignComponents.Contains(TrafficLight->GetSignId()))
  {
    TrafficSignComponents.Add(TrafficLight->GetSignId(), TrafficLight);
  }
}

const std::optional<carla::road::Map>& ATrafficLightManager::GetMap()
{
  return UCarlaStatics::GetGameMode(GetWorld())->GetMap();
}

void ATrafficLightManager::GenerateSignalsAndTrafficLights()
{
  EnsureDefaultModelsLoaded();

  if(!TrafficLightsGenerated)
  {
    if(!TrafficLightModel_RHT || !TrafficLightModel_LHT )
    {
      UE_LOG(LogCarla, Error, TEXT("Missing TrafficLightModel"));
      return;
    }

    RemoveRoadrunnerProps();

    SpawnTrafficLights();

    SpawnSignals();

    if (bAdjustSignsHeightToGround)
    {
      AdjustSpawnedSignsHeight();
    }

    TrafficLightsGenerated = true;
  }
}

TArray<AActor*> ATrafficLightManager::GetSignsToIgnoreWhileTracing() const
{
  TArray<AActor*> IgnoredActors;
  IgnoredActors.Reserve(TrafficSigns.Num());
  for (ATrafficSignBase* Sign : TrafficSigns)
  {
    if (IsValid(Sign))
    {
      IgnoredActors.Add(Sign);
    }
  }
  return IgnoredActors;
}

bool ATrafficLightManager::AdjustSpawnedSignsHeight()
{
  UWorld* World = GetWorld();
  const TArray<AActor*> IgnoredActors = GetSignsToIgnoreWhileTracing();
  const TArray<UPrimitiveComponent*> NoIgnoredComponents;
  bool bAnyAdjusted = false;
  int32 GroundNotFoundCount = 0;
  for (ATrafficSignBase* Sign : TrafficSigns)
  {
    if (TrafficSignHeightUtils::AdjustSignToGround(
            World, Sign, IgnoredActors, NoIgnoredComponents))
    {
      bAnyAdjusted = true;
    }
    else if (IsValid(Sign) && !Sign->bPositioned && Sign->bGeneratedFromOpenDRIVE)
    {
      // Hand-placed signs are skipped by design, so they are not failures.
      ++GroundNotFoundCount;
    }
  }
  if (GroundNotFoundCount > 0)
  {
    UE_LOG(LogCarla, Warning,
        TEXT("Could not find ground for %d traffic sign(s)"),
        GroundNotFoundCount);
  }
  return bAnyAdjusted;
}

void ATrafficLightManager::SetAdjustSignsHeightToGround(bool bEnabled)
{
  bAdjustSignsHeightToGround = bEnabled;

  // Signs are generated at map load, before a client can apply world settings,
  // so honour a late enable by re-snapping the already-spawned signs.
  SnapSignsToGround();
}

void ATrafficLightManager::SnapSignsToGround()
{
  // AdjustSpawnedSignsHeight is idempotent (the bPositioned flag guards each
  // sign), so calling this again is safe.
  if (bAdjustSignsHeightToGround && TrafficLightsGenerated &&
      AdjustSpawnedSignsHeight())
  {
    if (ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(GetWorld()))
    {
      GameMode->RegisterEnvironmentObjects();
    }
  }
}

void ATrafficLightManager::RemoveGeneratedSignalsAndTrafficLights()
{
  for(auto& Sign : TrafficSigns)
  {
    Sign->Destroy();
  }
  TrafficSigns.Empty();
  SpawnedSigns.Empty();
  bSpawnedSignsSuppressed = false;

  for(auto& TrafficGroup : TrafficGroups)
  {
    TrafficGroup.Value->Destroy();
  }
  TrafficGroups.Empty();

  TrafficControllers.Empty();

  TrafficLightsGenerated = false;
}

void ATrafficLightManager::MatchTrafficLightActorsWithOpenDriveSignals()
{
  TArray<AActor*> Actors;
  UGameplayStatics::GetAllActorsOfClass(GetWorld(), ATrafficLightBase::StaticClass(), Actors);

  std::string opendrive_xml = carla::rpc::FromFString(UOpenDrive::GetXODR(GetWorld()));
  auto Map = carla::opendrive::OpenDriveParser::Load(opendrive_xml);

  if (!Map)
  {
    carla::log_warning("Map not found");
    return;
  }

  TArray<ATrafficLightBase*> TrafficLights;
  for (AActor* Actor : Actors)
  {
    ATrafficLightBase* TrafficLight = Cast<ATrafficLightBase>(Actor);
    if (TrafficLight)
    {
      TrafficLight->GetTrafficLightComponent()->SetSignId("");
      TrafficLights.Add(TrafficLight);
    }
  }

  if (!TrafficLights.Num())
  {
    carla::log_warning("No actors in the map");
    return;
  }

  const auto& Signals = Map->GetSignals();
  const auto& Controllers = Map->GetControllers();

  for(const auto& Signal : Signals) {
    const auto& ODSignal = Signal.second;
    const FTransform Transform = ODSignal->GetTransform();
    const FVector Location = Transform.GetLocation();
    if (ODSignal->GetName() == "")
    {
      continue;
    }
    ATrafficLightBase* ClosestActor = TrafficLights.Top();
    float MinDistance = FVector::DistSquaredXY(TrafficLights.Top()->GetActorLocation(), Location);
    for (ATrafficLightBase* Actor : TrafficLights)
    {
      float Distance = FVector::DistSquaredXY(Actor->GetActorLocation(), Location);
      if (Distance < MinDistance)
      {
        MinDistance = Distance;
        ClosestActor = Actor;
      }
    }
    ATrafficLightBase* TrafficLight = ClosestActor;
    auto* Component = TrafficLight->GetTrafficLightComponent();
    if (Component->GetSignId() == "")
    {
      Component->SetSignId(carla::rpc::ToFString(ODSignal->GetSignalId()));
    }
    else
    {
      carla::log_warning("Could not find a suitable traffic light for signal", ODSignal->GetSignalId(),
          "Closest traffic light has id", carla::rpc::FromFString(Component->GetSignId()));
    }

  }
}

void ATrafficLightManager::InitializeTrafficLights()
{
  if (!GetMap())
  {
    carla::log_warning("Coud not generate traffic lights: missing map.");
    return;
  }

  EnsureDefaultModelsLoaded();

  FString MapName = GetWorld()->GetMapName();
  FString XODRPath = UOpenDrive::FindPathToXODRFile(MapName);
  bool bHasMapLogic = false;

  if (!XODRPath.IsEmpty())
  {
    FString DirectoryPath;
    FString Filename, Extension;
    FPaths::Split(XODRPath, DirectoryPath, Filename, Extension);
    FString JsonFilePath = FPaths::Combine(DirectoryPath, TEXT("map_logic.json"));
    bHasMapLogic = FPlatformFileManager::Get().GetPlatformFile().FileExists(*JsonFilePath);
  }

  if (!TrafficLightsGenerated)
  {
    if (bHasMapLogic)
    {
      RemoveRoadrunnerProps();
      SpawnSignals();
      TrafficLightsGenerated = true;
    }
    else
    {
      GenerateSignalsAndTrafficLights();
    }
  }

  if (!XODRPath.IsEmpty() && bHasMapLogic)
  {
    UMapLogicParser::ApplyLaneIdsFromMapLogic(XODRPath, this);
  }

  if (GetWorld()->GetWorldPartition() != nullptr)
  {
    SetActorTickEnabled(true);
    if (!LevelAddedHandle.IsValid())
    {
      LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(
          this, &ATrafficLightManager::OnLevelAddedToWorld);
    }
    for (ULevel *Level : GetWorld()->GetLevels())
    {
      SuppressSpawnedSignsIfLevelHasPlacedSigns(Level);
    }
  }
}

void ATrafficLightManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
  FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
  LevelAddedHandle.Reset();
  Super::EndPlay(EndPlayReason);
}

void ATrafficLightManager::OnLevelAddedToWorld(ULevel *Level, UWorld *World)
{
  if (World == GetWorld())
  {
    SuppressSpawnedSignsIfLevelHasPlacedSigns(Level);
  }
}

void ATrafficLightManager::SuppressSpawnedSignsIfLevelHasPlacedSigns(ULevel *Level)
{
  if (bSpawnedSignsSuppressed || Level == nullptr || SpawnedSigns.Num() == 0 ||
      !CVarCarlaHideSpawnedSignsWhenPlaced.GetValueOnGameThread())
  {
    return;
  }
  AActor *PlacedSign = nullptr;
  for (AActor *Actor : Level->Actors)
  {
    // Traffic lights are sign actors too, and are left out: this is about
    // the plates.
    if (IsValid(Actor) && Actor->IsA<ATrafficSignBase>() && !Actor->IsA<ATrafficLightBase>() &&
        Actor->GetOwner() != this && !SpawnedSigns.Contains(Cast<ATrafficSignBase>(Actor)))
    {
      PlacedSign = Actor;
      break;
    }
  }
  if (PlacedSign == nullptr)
  {
    return;
  }
  bSpawnedSignsSuppressed = true;
  int32 Hidden = 0;
  for (const TWeakObjectPtr<ATrafficSignBase> &Weak : SpawnedSigns)
  {
    ATrafficSignBase *Sign = Weak.Get();
    if (!IsValid(Sign))
    {
      continue;
    }
    Sign->SetActorHiddenInGame(true);
    TArray<UPrimitiveComponent*> Primitives;
    Sign->GetComponents(Primitives);
    for (UPrimitiveComponent *Primitive : Primitives)
    {
      // The trigger boxes carry the stop/yield/speed logic.
      if (!Primitive->IsA<UBoxComponent>())
      {
        Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
      }
    }
    ++Hidden;
  }
  UE_LOG(LogCarla, Log,
      TEXT("Traffic signs: %s is placed in the level, hiding the %d signs spawned from the OpenDRIVE"),
      *PlacedSign->GetName(), Hidden);
}

void ATrafficLightManager::AdoptModelConfigurationFrom(const ATrafficLightManager& Other)
{
  TrafficLightModel_RHT = Other.TrafficLightModel_RHT;
  TrafficLightModel_LHT = Other.TrafficLightModel_LHT;
  TrafficSignsModels = Other.TrafficSignsModels;
  SignComponentModels = Other.SignComponentModels;
  SpeedLimitModels = Other.SpeedLimitModels;
  bAdjustSignsHeightToGround = Other.bAdjustSignsHeightToGround;
}

void ATrafficLightManager::Tick(float DeltaSeconds)
{
  Super::Tick(DeltaSeconds);
  UpdateSignalGroundDormancy();
}

void ATrafficLightManager::UpdateSignalGroundDormancy()
{
  const int32 Num = TrafficSigns.Num();
  if (Num == 0)
  {
    return;
  }
  UWorld *World = GetWorld();
  // A World Partition map streams its ground in around the streaming source,
  // so a signal generated over an unloaded cell finds nothing to stand on and
  // keeps the height the OpenDRIVE record gave it. This sweep is already the
  // place that learns when the ground below a signal becomes resident, so the
  // snap rides it instead of running once at generation time.
  const bool bSnapToGround = bAdjustSignsHeightToGround;
  const TArray<AActor*> IgnoredActors =
      bSnapToGround ? GetSignsToIgnoreWhileTracing() : TArray<AActor*>();
  const TArray<UPrimitiveComponent*> NoIgnoredComponents;
  bool bSweepWrapped = false;
  const int32 Checks = FMath::Min(DormancyChecksPerTick, Num);
  for (int32 i = 0; i < Checks; ++i)
  {
    DormancySweepIndex = (DormancySweepIndex + 1) % Num;
    bSweepWrapped |= (DormancySweepIndex == 0);
    ATrafficSignBase *Sign = TrafficSigns[DormancySweepIndex];
    if (!IsValid(Sign))
    {
      continue;
    }
    // Hidden for good: the map's placed signs stand in for it.
    if (bSpawnedSignsSuppressed && SpawnedSigns.Contains(Sign))
    {
      continue;
    }
    const FVector Origin = Sign->GetActorLocation();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SignalGroundDormancy), false, Sign);
    FHitResult Hit;
    const bool bGroundResident = World->LineTraceSingleByChannel(
        Hit,
        Origin + FVector(0.0f, 0.0f, 50.0f),
        Origin - FVector(0.0f, 0.0f, DormancyTraceDepth),
        ECC_Visibility,
        Params);
    if (Sign->IsHidden() == bGroundResident)
    {
      Sign->SetActorHiddenInGame(!bGroundResident);
    }
    if (bSnapToGround && bGroundResident &&
        TrafficSignHeightUtils::AdjustSignToGround(
            World, Sign, IgnoredActors, NoIgnoredComponents))
    {
      bPendingEnvironmentObjectRefresh = true;
    }
  }

  // Re-registering walks every actor in the world, so it waits for the sweep
  // to come back round rather than running on every tick that moved a sign.
  if (bPendingEnvironmentObjectRefresh && bSweepWrapped)
  {
    bPendingEnvironmentObjectRefresh = false;
    if (ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(World))
    {
      GameMode->RegisterEnvironmentObjects();
    }
  }
}

bool MatchSignalAndActor(const carla::road::Signal &Signal, ATrafficSignBase* ClosestTrafficSign)
{
  namespace cr = carla::road;
  if (ClosestTrafficSign)
  {
    if ((Signal.GetType() == cr::SignalType::StopSign()) &&
        ClosestTrafficSign->GetTrafficSignState() == ETrafficSignState::StopSign)
    {
      return true;
    }
    else if ((Signal.GetType() == cr::SignalType::YieldSign()) &&
        ClosestTrafficSign->GetTrafficSignState() == ETrafficSignState::YieldSign)
    {
      return true;
    }
    else if (cr::SignalType::IsTrafficLight(Signal.GetType()))
    {
      if (ClosestTrafficSign->GetTrafficSignState() == ETrafficSignState::TrafficLightRed ||
          ClosestTrafficSign->GetTrafficSignState() == ETrafficSignState::TrafficLightYellow ||
          ClosestTrafficSign->GetTrafficSignState() == ETrafficSignState::TrafficLightGreen)
        return true;
    }
    else if (Signal.GetType() == cr::SignalType::MaximumSpeed())
    {
      // The subtype is the limit in km/h; the actor answers in km/h for every speed-limit
      // state (dedicated or custom), so any value the map carries can be matched.
      const int32 SignalKmh = FCString::Atoi(*FString(Signal.GetSubtype().c_str()));
      return SignalKmh > 0 && ClosestTrafficSign->GetSpeedLimitKmh() == SignalKmh;
    }
  }
  return false;
}

template<typename T = ATrafficSignBase>
T * GetClosestTrafficSignActor(const carla::road::Signal &Signal, UWorld* World)
{
  auto CarlaTransform = Signal.GetTransform();
  // The signal transform carries the OpenDRIVE zOffset (mounting height of
  // the plate above the road); baked sign actors have their pivot at road
  // level, so match against the road-level position.
  CarlaTransform.location.z -= static_cast<float>(Signal.GetZOffset());
  FTransform UETransform(CarlaTransform);
  FVector Location = UETransform.GetLocation();
  // max distance to match 500cm
  constexpr float MaxDistanceMatchSqr = 250000.0;
  T * ClosestTrafficSign = nullptr;
  TArray<AActor*> Actors;
  UGameplayStatics::GetAllActorsOfClass(World, T::StaticClass(), Actors);
  float MinDistance = MaxDistanceMatchSqr;
  for (AActor* Actor : Actors)
  {
    float Dist = FVector::DistSquared(Actor->GetActorLocation(), Location);
    T * TrafficSign = Cast<T>(Actor);
    // Baked twin signs carry an explicit identity. Their support may be moved
    // onto a sidewalk more than 5 m from the logical OpenDRIVE control point.
    // Never adopt another signal's identified sign through the proximity fallback.
    if (const AGeoTrafficSign* GeoSign = Cast<AGeoTrafficSign>(Actor))
    {
      if (!GeoSign->SignalId.IsEmpty())
      {
        if (GeoSign->SignalId == carla::rpc::ToFString(Signal.GetSignalId()) &&
            MatchSignalAndActor(Signal, TrafficSign))
        {
          return TrafficSign;
        }
        continue;
      }
    }
    if (Dist < MinDistance && MatchSignalAndActor(Signal, TrafficSign))
    {
      ClosestTrafficSign = TrafficSign;
      MinDistance = Dist;
    }
  }
  return ClosestTrafficSign;
}

void ATrafficLightManager::SpawnTrafficLights()
{
  namespace cr = carla::road;
  const auto& Signals = GetMap()->GetSignals();
  std::unordered_set<std::string> SignalsToSpawn;
  for(const auto& ControllerPair : GetMap()->GetControllers())
  {
    const auto& Controller = ControllerPair.second;
    for(const auto& SignalId : Controller->GetSignals())
    {
      auto& Signal = Signals.at(SignalId);
      if (!cr::SignalType::IsTrafficLight(Signal->GetType()))
      {
        continue;
      }
      ATrafficLightBase * TrafficLight = GetClosestTrafficSignActor<ATrafficLightBase>(*Signal, GetWorld());
      if (TrafficLight)
      {
        UTrafficLightComponent *TrafficLightComponent = TrafficLight->GetTrafficLightComponent();
        TrafficLightComponent->SetSignId(SignalId.c_str());
      }
      else
      {
        SignalsToSpawn.insert(SignalId);
      }
    }
  }

  for(const auto& SignalPair : Signals)
  {
    const auto& SignalId = SignalPair.first;
    const auto& Signal = SignalPair.second;
    if(!Signal->GetControllers().size() &&
       !GetMap()->IsJunction(Signal->GetRoadId()) &&
       carla::road::SignalType::IsTrafficLight(Signal->GetType()) &&
       !SignalsToSpawn.count(SignalId))
    {
      ATrafficLightBase * TrafficLight = GetClosestTrafficSignActor<ATrafficLightBase>(*Signal, GetWorld());
      if (TrafficLight)
      {
        UTrafficLightComponent *TrafficLightComponent = TrafficLight->GetTrafficLightComponent();
        TrafficLightComponent->SetSignId(SignalId.c_str());
      }
      else
      {
        SignalsToSpawn.insert(SignalId);
      }
    }
  }

  ACarlaGameModeBase *GM = UCarlaStatics::GetGameMode(GetWorld());
  check(GM);
  for(auto &SignalId : SignalsToSpawn)
  {
    // TODO: should this be an assert?
    // RELEASE_ASSERT(
    //     Signals.count(SignalId) > 0,
    //     "Reference to inexistent signal. Possible OpenDRIVE error.");
    if (Signals.count(SignalId) == 0)
    {
      UE_LOG(LogCarla, Warning,
          TEXT("Possible OpenDRIVE error, reference to nonexistent signal id: %s"),
          *carla::rpc::ToFString(SignalId));
      continue;
    }
    const auto& Signal = Signals.at(SignalId);
    auto CarlaTransform = Signal->GetTransform();
    // The signal transform carries the OpenDRIVE zOffset (mounting height of
    // the reference point above the road). The spawned blueprint models the
    // whole signal with its pivot at the pole base, so spawn at road level.
    CarlaTransform.location.z -= static_cast<float>(Signal->GetZOffset());
    auto ClosestWaypointToSignal =
        GetMap()->GetClosestWaypointOnRoad(CarlaTransform.location);

    const bool IsRHT = GetMap()->GetLane(ClosestWaypointToSignal.value()).GetRoad()->IsRHT();

    FTransform SpawnTransform(CarlaTransform);

    FVector SpawnLocation = SpawnTransform.GetLocation();
    FRotator SpawnRotation(SpawnTransform.GetRotation());
    // Blueprints are all rotated by 90 degrees
    SpawnRotation.Yaw += 90;
    // Remove road inclination
    SpawnRotation.Roll = 0;
    SpawnRotation.Pitch = 0;

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParams.OverrideLevel = GM->GetULevelFromName("TrafficLights");

    auto TrafficLightModel = IsRHT ? TrafficLightModel_RHT : TrafficLightModel_LHT;
    ATrafficLightBase * TrafficLight = GetWorld()->SpawnActor<ATrafficLightBase>(
        TrafficLightModel,
        SpawnLocation,
        SpawnRotation,
        SpawnParams);

    // Tagged here, at the only site that spawns a light, so height adjustment
    // never moves a light placed by hand in the level (the matched-actor path
    // above also adds pre-existing actors to TrafficSigns).
    TrafficLight->bGeneratedFromOpenDRIVE = true;
    TrafficSigns.Add(TrafficLight);

    UTrafficLightComponent *TrafficLightComponent = TrafficLight->GetTrafficLightComponent();
    TrafficLightComponent->SetSignId(SignalId.c_str());

    if (ClosestWaypointToSignal)
    {
      auto SignalDistanceToRoad =
          (GetMap()->ComputeTransform(ClosestWaypointToSignal.value()).location - CarlaTransform.location).Length();
      double LaneWidth = GetMap()->GetLaneWidth(ClosestWaypointToSignal.value());

      if(SignalDistanceToRoad < LaneWidth * 0.5)
      {
        carla::log_warning("Traffic light",
            TCHAR_TO_UTF8(*TrafficLightComponent->GetSignId()),
            "overlaps a driving lane. Disabling collision...");

        TArray<UPrimitiveComponent*> Primitives;
        TrafficLight->GetComponents(Primitives);
        for (auto* Primitive : Primitives)
        {
          Primitive->SetCollisionProfileName(TEXT("NoCollision"));
        }
      }
    }

    RegisterLightComponentFromOpenDRIVE(TrafficLightComponent);
    TrafficLightComponent->InitializeSign(GetMap().value());
  }
}

void ATrafficLightManager::SpawnSignals()
{
  ACarlaGameModeBase *GM = UCarlaStatics::GetGameMode(GetWorld());
  check(GM);

  const auto &Signals = GetMap()->GetSignals();
  for (auto& SignalPair : Signals)
  {
    auto &Signal = SignalPair.second;
    FString SignalType = Signal->GetType().c_str();

    ATrafficSignBase * ClosestTrafficSign = GetClosestTrafficSignActor(*Signal, GetWorld());
    if (ClosestTrafficSign)
    {
      USignComponent *SignComponent;
      if (SignComponentModels.Contains(SignalType))
      {
          SignComponent =
              NewObject<USignComponent>(
              ClosestTrafficSign, SignComponentModels[SignalType]);
      }
      else
      {
        SignComponent =
              NewObject<USignComponent>(
              ClosestTrafficSign);
      }
      SignComponent->SetSignId(Signal->GetSignalId().c_str());
      SignComponent->RegisterComponent();
      SignComponent->AttachToComponent(
          ClosestTrafficSign->GetRootComponent(),
          FAttachmentTransformRules::KeepRelativeTransform);
      TrafficSignComponents.Add(SignComponent->GetSignId(), SignComponent);
      TrafficSigns.Add(ClosestTrafficSign);
    }
    else if (TrafficSignsModels.Contains(SignalType))
    {
      // We do not spawn stops painted in the ground
      if (Signal->GetName() == "Stencil_STOP")
      {
        continue;
      }
      auto CarlaTransform = Signal->GetTransform();
      // The signal transform carries the OpenDRIVE zOffset (mounting height
      // of the plate above the road, e.g. ~2.3 m for Town12 stop signs). The
      // spawned blueprint models the whole sign with its pivot at the pole
      // base, so spawn at road level or the sign floats by exactly zOffset.
      CarlaTransform.location.z -= static_cast<float>(Signal->GetZOffset());
      FTransform SpawnTransform(CarlaTransform);
      FVector SpawnLocation = SpawnTransform.GetLocation();
      FRotator SpawnRotation(SpawnTransform.GetRotation());
      SpawnRotation.Yaw += 90;
      // Remove road inclination
      SpawnRotation.Roll = 0;
      SpawnRotation.Pitch = 0;

      FActorSpawnParameters SpawnParams;
      SpawnParams.Owner = this;
      SpawnParams.SpawnCollisionHandlingOverride =
          ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
      SpawnParams.OverrideLevel = GM->GetULevelFromName("TrafficSigns");
      ATrafficSignBase * TrafficSign = GetWorld()->SpawnActor<ATrafficSignBase>(
          TrafficSignsModels[SignalType],
          SpawnLocation,
          SpawnRotation,
          SpawnParams);

      USignComponent *SignComponent =
          NewObject<USignComponent>(TrafficSign, SignComponentModels[SignalType]);
      SignComponent->SetSignId(Signal->GetSignalId().c_str());
      SignComponent->RegisterComponent();
      SignComponent->AttachToComponent(
          TrafficSign->GetRootComponent(),
          FAttachmentTransformRules::KeepRelativeTransform);
      SignComponent->InitializeSign(GetMap().value());

      auto ClosestWaypointToSignal =
          GetMap()->GetClosestWaypointOnRoad(CarlaTransform.location);
      if (ClosestWaypointToSignal)
      {
        auto SignalDistanceToRoad =
            (GetMap()->ComputeTransform(ClosestWaypointToSignal.value()).location - CarlaTransform.location).Length();
        double LaneWidth = GetMap()->GetLaneWidth(ClosestWaypointToSignal.value());

        if(SignalDistanceToRoad < LaneWidth * 0.5)
        {
          carla::log_warning("Traffic sign",
              TCHAR_TO_UTF8(*SignComponent->GetSignId()),
              "overlaps a driving lane. Disabling collision...");

          TArray<UPrimitiveComponent*> Primitives;
          TrafficSign->GetComponents(Primitives);
          for (auto* Primitive : Primitives)
          {
            Primitive->SetCollisionProfileName(TEXT("NoCollision"));
          }
        }
      }
      TrafficSignComponents.Add(SignComponent->GetSignId(), SignComponent);
      // Only spawned signs are tagged: the matched-actor branch above adds
      // hand-placed level actors to TrafficSigns and must not be moved.
      TrafficSign->bGeneratedFromOpenDRIVE = true;
      TrafficSigns.Add(TrafficSign);
      SpawnedSigns.Add(TrafficSign);
    }
    else if (Signal->GetType() == carla::road::SignalType::MaximumSpeed() &&
            SpeedLimitModels.Contains(Signal->GetSubtype().c_str()))
    {
      // Geo-style switch: the OpenDRIVE subtype is always km/h (SignalType.h StVO
      // vocabulary), but a map whose signals carry country="US" should show MUTCD mph
      // plates. Convert and snap to the nearest available plate; fall back to the EU
      // model when no US plate resolves.
      TSubclassOf<AActor> SpeedModel = SpeedLimitModels[Signal->GetSubtype().c_str()];
      if (Signal->GetCountry() == "US" && SpeedLimitModels_US.Num() > 0)
      {
        const float Kmh = FCString::Atof(*FString(Signal->GetSubtype().c_str()));
        const float Mph = Kmh / 1.60934f;
        FString BestKey;
        float BestDiff = TNumericLimits<float>::Max();
        for (const auto &Pair : SpeedLimitModels_US)
        {
          const float Diff = FMath::Abs(FCString::Atof(*Pair.Key) - Mph);
          if (Diff < BestDiff)
          {
            BestDiff = Diff;
            BestKey = Pair.Key;
          }
        }
        if (!BestKey.IsEmpty())
        {
          SpeedModel = SpeedLimitModels_US[BestKey];
        }
      }
      auto CarlaTransform = Signal->GetTransform();
      // Spawn at road level: the blueprint pivot is at the pole base (see
      // the zOffset note on the traffic-sign branch above).
      CarlaTransform.location.z -= static_cast<float>(Signal->GetZOffset());
      FTransform SpawnTransform(CarlaTransform);
      FVector SpawnLocation = SpawnTransform.GetLocation();
      FRotator SpawnRotation(SpawnTransform.GetRotation());
      SpawnRotation.Yaw += 90;
      // Remove road inclination
      SpawnRotation.Roll = 0;
      SpawnRotation.Pitch = 0;

      FActorSpawnParameters SpawnParams;
      SpawnParams.Owner = this;
      SpawnParams.SpawnCollisionHandlingOverride =
          ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
      SpawnParams.OverrideLevel = GM->GetULevelFromName("TrafficSigns");
      ATrafficSignBase * TrafficSign = GetWorld()->SpawnActor<ATrafficSignBase>(
          SpeedModel,
          SpawnLocation,
          SpawnRotation,
          SpawnParams);

      USpeedLimitComponent *SignComponent =
          NewObject<USpeedLimitComponent>(TrafficSign);
      SignComponent->SetSignId(Signal->GetSignalId().c_str());
      SignComponent->RegisterComponent();
      SignComponent->AttachToComponent(
          TrafficSign->GetRootComponent(),
          FAttachmentTransformRules::KeepRelativeTransform);
      SignComponent->InitializeSign(GetMap().value());
      SignComponent->SetSpeedLimit(Signal->GetValue());

      auto ClosestWaypointToSignal =
          GetMap()->GetClosestWaypointOnRoad(CarlaTransform.location);
      if (ClosestWaypointToSignal)
      {
        auto SignalDistanceToRoad =
            (GetMap()->ComputeTransform(ClosestWaypointToSignal.value()).location - CarlaTransform.location).Length();
        double LaneWidth = GetMap()->GetLaneWidth(ClosestWaypointToSignal.value());

        if(SignalDistanceToRoad < LaneWidth * 0.5)
        {
          carla::log_warning("Traffic sign",
              TCHAR_TO_UTF8(*SignComponent->GetSignId()),
              "overlaps a driving lane. Disabling collision...");

          TArray<UPrimitiveComponent*> Primitives;
          TrafficSign->GetComponents(Primitives);
          for (auto* Primitive : Primitives)
          {
            Primitive->SetCollisionProfileName(TEXT("NoCollision"));
          }
        }
      }
      TrafficSignComponents.Add(SignComponent->GetSignId(), SignComponent);
      // Only spawned signs are tagged: the matched-actor branch above adds
      // hand-placed level actors to TrafficSigns and must not be moved.
      TrafficSign->bGeneratedFromOpenDRIVE = true;
      TrafficSigns.Add(TrafficSign);
      SpawnedSigns.Add(TrafficSign);
    }
  }
}

void ATrafficLightManager::SetFrozen(bool InFrozen)
{
  bTrafficLightsFrozen = InFrozen;
  if (bTrafficLightsFrozen)
  {
    for (auto& TrafficGroupPair : TrafficGroups)
    {
      auto& TrafficGroup = TrafficGroupPair.Value;
      TrafficGroup->SetFrozenGroup(true);
    }
  }
  else
  {
    for (auto& TrafficGroupPair : TrafficGroups)
    {
      auto& TrafficGroup = TrafficGroupPair.Value;
      TrafficGroup->SetFrozenGroup(false);
    }
  }
}

bool ATrafficLightManager::GetFrozen()
{
  return bTrafficLightsFrozen;
}

ATrafficLightGroup* ATrafficLightManager::GetTrafficGroup(carla::road::JuncId JunctionId)
{
  if (TrafficGroups.Contains(JunctionId))
  {
    return TrafficGroups[JunctionId];
  }
  return nullptr;
}


UTrafficLightController* ATrafficLightManager::GetController(FString ControllerId)
{
  if (TrafficControllers.Contains(ControllerId))
  {
    return TrafficControllers[ControllerId];
  }
  return nullptr;
}

USignComponent* ATrafficLightManager::GetTrafficSign(FString SignId)
{
  if (!TrafficSignComponents.Contains(SignId))
  {
    return nullptr;
  }
  return TrafficSignComponents[SignId];
}

void ATrafficLightManager::RemoveRoadrunnerProps() const
{
    TArray<AActor*> Actors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), Actors);

    // Detect PropsNode Actor which is the father of all the Props imported from Roadrunner
    AActor* PropsNode = nullptr;
    for(AActor* Actor : Actors)
    {
      const FString Name = UKismetSystemLibrary::GetDisplayName(Actor);
      if(Name.Equals("PropsNode"))
      {
        PropsNode = Actor;
        break;
      }
    }

    if(PropsNode)
    {
      PropsNode->GetAttachedActors(Actors, true);
      RemoveAttachedProps(Actors);
      PropsNode->Destroy();
    }

}

void ATrafficLightManager::RemoveAttachedProps(TArray<AActor*> Actors) const
{
  for(AActor* Actor : Actors)
  {
    TArray<AActor*> AttachedActors;
    Actor->GetAttachedActors(AttachedActors, true);
    RemoveAttachedProps(AttachedActors);
    Actor->Destroy();
  }
}
