#include "BattleGameMode.h"
#include "BattleCameraPawn.h"
#include "BattleHUD.h"
#include "BattleManager.h"
#include "BattleSpawnerActor.h"
#include "EngineUtils.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Misc/CommandLine.h"
#include "UnrealClient.h"
#include "TimerManager.h"

ABattleGameMode::ABattleGameMode()
{
	DefaultPawnClass = ABattleCameraPawn::StaticClass();
	HUDClass = ABattleHUD::StaticClass();
}

void ABattleGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Ensure exactly one BattleManager exists. If the level designer placed one
	// (to tune BattlefieldRadius/centre per map), use it; otherwise spawn a
	// default so the game loop (AI + victory) always runs.
	UWorld* World = GetWorld();
	if (!World) return;

	bool bHasManager = false;
	for (TActorIterator<ABattleManager> It(World); It; ++It)
	{
		bHasManager = true;
		break;
	}
	if (!bHasManager)
	{
		World->SpawnActor<ABattleManager>(ABattleManager::StaticClass(),
			FTransform::Identity);
	}

	// ── -BiedaAutoTest: unattended render test harness ──────────────────────
	// Launch with:  UnrealEditor.exe <uproject> <map> -game -BiedaAutoTest
	// Spawns one squad in front of the default camera and adds lights (for
	// empty maps). Screenshots + auto-quit live in the module hook
	// (Bieda_total_war.cpp) so they work on any map. Zero effect without
	// the flag.
	if (FParse::Param(FCommandLine::Get(), TEXT("BiedaAutoTest")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] starting: spawning test squad + lights"));

		// Lights (test maps are empty — without these the frame is black)
		{
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(
				FVector::ZeroVector, FRotator(-45.f, 30.f, 0.f), P);
			if (Sun) Sun->SetMobility(EComponentMobility::Movable);
			ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, P);
			if (Sky && Sky->GetLightComponent())
			{
				Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
				Sky->GetLightComponent()->SetIntensity(1.f);
			}
		}

		// Test squad — directly in front of the default camera (origin, +X)
		{
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ABattleSpawnerActor* S = World->SpawnActor<ABattleSpawnerActor>(
				FVector(2500.f, 0.f, 0.f), FRotator(0.f, 180.f, 0.f), P);
			UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] spawner=%s"), S ? *S->GetName() : TEXT("FAILED"));
		}

		// Screenshots + quit — on the core ticker (real time), NOT world timers:
		// the MainMenu phase pauses the game, so world timers never fire.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[World, Elapsed = 0.f, Shots = 0](float Dt) mutable -> bool
			{
				Elapsed += Dt;
				if (Shots == 0 && Elapsed > 5.f)
				{ ++Shots; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 1")); FScreenshotRequest::RequestScreenshot(false); }
				else if (Shots == 1 && Elapsed > 9.f)
				{ ++Shots; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 2")); FScreenshotRequest::RequestScreenshot(false); }
				else if (Shots == 2 && Elapsed > 13.f)
				{
					++Shots;
					UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] done — quitting"));
					FPlatformMisc::RequestExit(false);
					return false;
				}
				return true;
			}));
	}
}
