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
	// Mesh pivot is already at the feet (verified: lowest vertex ~5cm above
	// origin, same convention as the ISM Soldier_Retopo mesh) — no Z lift needed.
	// Mesh's authored forward axis is rotated 90° relative to the capsule's
	// forward; the ISM path (raw FBX import, no Blender re-export) needs zero
	// correction and faces correctly, so this offset compensates for the
	// Blender FBX round-trip used for the animated variant.
	MeshComp->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
	MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	MeshComp->SetCastShadow(true);
	// Force the pose to advance every frame regardless of on-screen size /
	// occlusion. The default (OnlyTickPoseWhenRendered) + these being small,
	// hidden-then-shown mass-battle actors left the single-node animation
	// frozen on frame 0 (arms-down pose applied, but never advancing → no
	// stepping). AlwaysTickPose guarantees playback while the actor is visible.
	MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	MeshComp->bEnableUpdateRateOptimizations = false;
	// Component must tick even though the owning actor doesn't (we teleport it).
	MeshComp->PrimaryComponentTick.bCanEverTick = true;
	MeshComp->PrimaryComponentTick.bStartWithTickEnabled = true;

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
	USkeletalMeshComponent* MeshComp = GetMesh();

	if (NewState != CurrentState)
	{
		CurrentState = NewState;
		if (UAnimSequence* Seq = GetSequenceForState(CurrentState))
		{
			// PlayAnimation() = set single-node mode + anim + start playing in
			// one call (more reliable than SetAnimation+Play). Death plays once
			// and holds its collapsed final pose; everything else loops.
			MeshComp->PlayAnimation(Seq, /*bLooping=*/ CurrentState != EAgentState::DEAD);
		}
		return;
	}

	// Same state: guard against the very first sync frame having run before the
	// mesh was ready (PlayAnimation would have no-opped, then the state-change
	// branch never re-fires). Re-assert looping playback if it isn't playing.
	if (CurrentState != EAgentState::DEAD && !MeshComp->IsPlaying())
	{
		if (UAnimSequence* Seq = GetSequenceForState(CurrentState))
			MeshComp->PlayAnimation(Seq, /*bLooping=*/ true);
	}
}
