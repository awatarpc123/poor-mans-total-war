#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BattleDeployZone.generated.h"

class UBoxComponent;

/**
 * A rectangular deployment zone, placed in the editor — one per side (TeamId).
 * During the Deploy phase its outline is drawn on the ground so the player can
 * see where to position that side's units before starting the battle.
 *
 * Set the size by scaling the actor or editing the box extent. ContainsPoint()
 * is provided for future hard-clamping of unit placement to the zone.
 */
UCLASS()
class BIEDA_TOTAL_WAR_API ABattleDeployZone : public AActor
{
	GENERATED_BODY()

public:
	ABattleDeployZone();

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Deploy")
	UBoxComponent* Zone;

	/** Which side this zone belongs to (0 = player). Drives the outline colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deploy")
	uint8 TeamId = 0;

	/** Half-width of the zone along its local Y (cm). Convenience for hooking a
	 *  spawner's MaxDeployWidth to the zone later. */
	float GetDeployWidth() const;

	/** True if WorldPos (2D, in the zone's local frame) lies inside the box. */
	bool ContainsPoint(const FVector& WorldPos) const;
};
