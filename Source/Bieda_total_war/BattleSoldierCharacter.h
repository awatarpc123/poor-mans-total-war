#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Animation/AnimSequence.h"
#include "BattleTypes.h"
#include "BattleSoldierCharacter.generated.h"

/**
 * Animated soldier actor — used ONLY by the hybrid rendering path in
 * ABattleSpawnerActor: the currently selected squad, or any squad within
 * camera range, is rendered with real Characters + skeletal animation
 * instead of the cheap static ISM. Everyone else stays on ISM. Bounded
 * count by design — never spawn one of these per soldier in the whole army.
 *
 * Mesh/animations are bound to Soldier_Animated_Skeleton (the heavier,
 * ~134k-tri model) because that's what the 8 baked animations were exported
 * against; Soldier_20K (used for the ISM bulk) is a different, incompatible
 * skeleton. Tolerable here since the actor count is capped by
 * AnimatedActorRange, unlike rendering the whole army this way.
 */
UCLASS()
class BIEDA_TOTAL_WAR_API ABattleSoldierCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ABattleSoldierCharacter();

	/** Switches animation immediately (no blending — matches the game's
	 *  discrete state machine, snap cuts are expected, not smooth blends). */
	void SetSoldierState(EAgentState NewState);

private:
	UPROPERTY()
	TObjectPtr<UAnimSequence> IdleAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> MarchAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> RunAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> FleeAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> DieAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> ShootStandingAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> ReloadAnim;

	// Loaded but unused for now — reserved for a future kneeling front-rank
	// visual mode (2-line formations), not yet a distinct EAgentState.
	UPROPERTY()
	TObjectPtr<UAnimSequence> KneelAnim;

	UPROPERTY()
	TObjectPtr<UAnimSequence> ShootKneelingAnim;

	EAgentState CurrentState = EAgentState::DEAD;   // sentinel: forces first SetSoldierState call to apply
	UAnimSequence* GetSequenceForState(EAgentState State) const;
};
