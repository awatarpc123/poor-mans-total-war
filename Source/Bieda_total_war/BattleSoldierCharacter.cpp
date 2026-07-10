#include "BattleSoldierCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

ABattleSoldierCharacter::ABattleSoldierCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	// Not driven by CharacterMovementComponent — BattleSpawnerActor teleports
	// these via SetActorLocationAndRotation to match the Mass Entity
	// transform. Disable movement/capsule collision entirely to avoid paying
	// for physics we never use.
	GetCharacterMovement()->SetComponentTickEnabled(false);
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	USkeletalMeshComponent* MeshComp = GetMesh();
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	MeshComp->SetCastShadow(true);

	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshFinder(
		TEXT("/Game/Models/Soldier_Animated/Soldier_Animated.Soldier_Animated"));
	if (MeshFinder.Succeeded())
		MeshComp->SetSkeletalMesh(MeshFinder.Object);

	static ConstructorHelpers::FObjectFinder<UAnimSequence> IdleFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Idle.AS_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> MarchFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_March.AS_March"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> RunFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Run.AS_Run"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> FleeFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Flee.AS_Flee"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> DieFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Die.AS_Die"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> ShootStandFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Shoot_Standing.AS_Shoot_Standing"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> KneelFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Kneel.AS_Kneel"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> ShootKneelFinder(
		TEXT("/Game/Models/Soldier_Animated/AS_Shoot_Kneeling.AS_Shoot_Kneeling"));

	if (IdleFinder.Succeeded())        IdleAnim           = IdleFinder.Object;
	if (MarchFinder.Succeeded())       MarchAnim          = MarchFinder.Object;
	if (RunFinder.Succeeded())         RunAnim            = RunFinder.Object;
	if (FleeFinder.Succeeded())        FleeAnim           = FleeFinder.Object;
	if (DieFinder.Succeeded())         DieAnim            = DieFinder.Object;
	if (ShootStandFinder.Succeeded())  ShootStandingAnim  = ShootStandFinder.Object;
	if (KneelFinder.Succeeded())       KneelAnim          = KneelFinder.Object;
	if (ShootKneelFinder.Succeeded())  ShootKneelingAnim  = ShootKneelFinder.Object;
}

UAnimSequence* ABattleSoldierCharacter::GetSequenceForState(EAgentState State) const
{
	switch (State)
	{
	case EAgentState::HOLDING:
	case EAgentState::LOADING:
	case EAgentState::SHAKEN:
	case EAgentState::PINNED:
		return IdleAnim;
	case EAgentState::ADVANCING:
		return MarchAnim;
	case EAgentState::MELEE:
		return RunAnim;
	case EAgentState::ROUTING:
	case EAgentState::RALLYING:
		return FleeAnim;
	case EAgentState::AIMING:
	case EAgentState::FIRING:
		return ShootStandingAnim;
	case EAgentState::DEAD:
		return DieAnim;
	default:
		return IdleAnim;
	}
}

void ABattleSoldierCharacter::SetSoldierState(EAgentState NewState)
{
	if (NewState == CurrentState) return;
	CurrentState = NewState;

	if (UAnimSequence* Seq = GetSequenceForState(CurrentState))
	{
		USkeletalMeshComponent* MeshComp = GetMesh();
		MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		MeshComp->SetAnimation(Seq);
		// Death anim plays once and holds its final (collapsed) pose;
		// everything else loops.
		MeshComp->Play(CurrentState != EAgentState::DEAD);
	}
}
