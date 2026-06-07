#include "BattleDeployZone.h"
#include "Components/BoxComponent.h"
#include "BattleManager.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

ABattleDeployZone::ABattleDeployZone()
{
	PrimaryActorTick.bCanEverTick = true;

	Zone = CreateDefaultSubobject<UBoxComponent>(TEXT("Zone"));
	RootComponent = Zone;
	Zone->SetBoxExtent(FVector(3000.f, 2000.f, 200.f));   // default ~60x40m footprint
	Zone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Zone->ShapeColor = FColor(40, 160, 255);
}

void ABattleDeployZone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (!World) return;

	// Outline is only meaningful while deploying — find the manager's phase.
	EGamePhase Phase = EGamePhase::Battle;
	for (TActorIterator<ABattleManager> It(World); It; ++It) { Phase = It->GetGamePhase(); break; }
	if (Phase != EGamePhase::Deploy) return;

	const FColor C = (TeamId == 0) ? FColor(40, 160, 255) : FColor(255, 80, 60);
	DrawDebugBox(World, GetActorLocation(), Zone->GetScaledBoxExtent(),
		GetActorQuat(), C, /*bPersistent*/ false, -1.f, 0, /*thickness*/ 30.f);
}

float ABattleDeployZone::GetDeployWidth() const
{
	// Full width along local Y = 2 * scaled extent Y.
	return Zone ? Zone->GetScaledBoxExtent().Y * 2.f : 0.f;
}

bool ABattleDeployZone::ContainsPoint(const FVector& WorldPos) const
{
	if (!Zone) return false;
	const FVector Local = GetActorTransform().InverseTransformPosition(WorldPos);
	const FVector Ext   = Zone->GetScaledBoxExtent();
	return FMath::Abs(Local.X) <= Ext.X && FMath::Abs(Local.Y) <= Ext.Y;
}
