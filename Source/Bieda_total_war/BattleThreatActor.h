#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BattleThreatActor.generated.h"

/**
 * Marker placed in the level that represents a dangerous zone (enemy fire, artillery, etc.).
 * Soldiers inside ThreatRadius suffer heavy morale drain every second.
 * Visualised as a red wireframe sphere in debug mode.
 */
UCLASS()
class BIEDA_TOTAL_WAR_API ABattleThreatActor : public AActor
{
	GENERATED_BODY()

public:
	ABattleThreatActor();

	/** Radius of the danger zone in cm (default 500 = 5 m, same as officer aura) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Threat")
	float ThreatRadius = 500.f;

	/** Morale drained per second from every soldier inside ThreatRadius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Threat")
	float MoraleDrainRate = 25.f;
};
