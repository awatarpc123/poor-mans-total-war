#include "BattleDebugProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleThreatActor.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"

// Master switch for ALL battle debug rendering (capsules, morale bars, velocity
// arrows, rings, threat spheres). This is the single biggest editor-time cost at
// high agent counts: thousands of DrawDebug calls per frame. Toggle at runtime
// from the console:  bieda.Debug 0  (off) / bieda.Debug 1 (on). Default OFF so
// builds and perf tests run clean; flip to 1 when you want the diagnostic view.
static TAutoConsoleVariable<int32> CVarBiedaDebug(
	TEXT("bieda.Debug"),
	1,   // default ON so the debug shapes show without typing it each run;
	     // set `bieda.Debug 0` for a perf pass (debug draw is costly at 1000+).
	TEXT("Draw Bieda battle debug shapes (0=off, 1=on). Costs a lot at 1000+ agents."),
	ECVF_Default);

// Shared accessor (declared in the header) so every DrawDebug site across the
// module can gate on the same switch.
bool BiedaDebugDrawEnabled()
{
	return CVarBiedaDebug.GetValueOnGameThread() != 0;
}

UBattleDebugProcessor::UBattleDebugProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::FrameEnd;
	bRequiresGameThreadExecution = true;
}

void UBattleDebugProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	DebugQuery.Initialize(EntityManager);
	DebugQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	DebugQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	DebugQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	DebugQuery.AddRequirement<FAgentVelocityFragment>(EMassFragmentAccess::ReadOnly);
	DebugQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	DebugQuery.RegisterWithProcessor(*this);

	OfficerDebugQuery.Initialize(EntityManager);
	OfficerDebugQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	OfficerDebugQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	OfficerDebugQuery.AddRequirement<FOfficerFragment>(EMassFragmentAccess::ReadOnly);
	OfficerDebugQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	OfficerDebugQuery.RegisterWithProcessor(*this);

	NCODebugQuery.Initialize(EntityManager);
	NCODebugQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	NCODebugQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	NCODebugQuery.AddRequirement<FNCOFragment>(EMassFragmentAccess::ReadOnly);
	NCODebugQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	NCODebugQuery.RegisterWithProcessor(*this);

	DrummerDebugQuery.Initialize(EntityManager);
	DrummerDebugQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	DrummerDebugQuery.AddRequirement<FDrummerFragment>(EMassFragmentAccess::ReadOnly);
	DrummerDebugQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	DrummerDebugQuery.RegisterWithProcessor(*this);
}

static FColor AgentColor(EAgentState State)
{
	switch (State)
	{
	case EAgentState::ADVANCING: return FColor::Green;
	case EAgentState::HOLDING:   return FColor::White;
	case EAgentState::LOADING:   return FColor::Cyan;
	case EAgentState::AIMING:    return FColor(255, 165,   0);  // Orange
	case EAgentState::FIRING:    return FColor::Yellow;
	case EAgentState::MELEE:     return FColor::Magenta;
	case EAgentState::ROUTING:   return FColor::Red;
	case EAgentState::RALLYING:  return FColor(128,   0, 128);  // Purple
	case EAgentState::SHAKEN:    return FColor(255,  60,   0);  // Red-orange
	case EAgentState::WAVERING:  return FColor(255, 140,   0);  // Orange-yellow
	case EAgentState::STEADY:    return FColor(  0, 180, 140);  // Green-blue
	case EAgentState::PINNED:    return FColor::Blue;
	case EAgentState::DEAD:      return FColor(128, 128, 128);  // Gray
	default:                     return FColor::White;
	}
}

void UBattleDebugProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	// Skip the whole pass when debug drawing is off — no per-agent work at all.
	if (!BiedaDebugDrawEnabled()) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// ── Threat zones ────────────────────────────────────────────────────────
	for (TActorIterator<ABattleThreatActor> It(World); It; ++It)
	{
		const FVector Pos    = It->GetActorLocation();
		const float   Radius = It->ThreatRadius;

		// Red wireframe sphere (danger zone boundary)
		DrawDebugSphere(World, Pos, Radius, 16, FColor::Red, false, -1.f, 0, 2.f);

		// Red capsule at center so it's visible from far away
		DrawDebugCapsule(World, Pos + FVector(0.f, 0.f, 75.f), 50.f, 25.f,
			FQuat::Identity, FColor::Red, false, -1.f, 0, 4.f);
	}

	// ── Officers ────────────────────────────────────────────────────────────
	OfficerDebugQuery.ForEachEntityChunk(Context, [World](FMassExecutionContext& Ctx)
	{
		const auto Transforms  = Ctx.GetFragmentView<FTransformFragment>();
		const auto Morales     = Ctx.GetFragmentView<FMoraleFragment>();
		const auto OfficerData = Ctx.GetFragmentView<FOfficerFragment>();
		const auto Factions    = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			const FVector Pos    = Transforms[i].GetTransform().GetLocation();
			const FVector Up     = FVector(0.f, 0.f, 90.f);
			const float   Morale = Morales[i].Morale;
			const bool    bAlive = OfficerData[i].bIsAlive;
			const bool    bEnemy = Factions[i].TeamId != 0;

			// Large capsule — blue for player, dark red for enemy
			const FColor AliveColor = bEnemy ? FColor(200, 50, 50) : FColor::Blue;
			const FColor DeadColor  = bEnemy ? FColor(80, 0, 0)    : FColor(0, 0, 80);
			const FColor CapsuleColor = bAlive ? AliveColor : DeadColor;
			DrawDebugCapsule(World, Pos + Up, 80.f, 35.f, FQuat::Identity,
				CapsuleColor, false, -1.f, 0, 3.f);

			// Morale aura sphere (wireframe, yellow) when alive
			if (bAlive)
			{
				DrawDebugSphere(World, Pos, OfficerData[i].MoraleRadius,
					16, FColor::Yellow, false, -1.f, 0, 1.5f);
			}

			// Morale bar (same as soldiers but taller)
			if (bAlive)
			{
				const FVector BarBase = Pos + FVector(0.f, 0.f, 180.f);
				const FVector BarTop  = BarBase + FVector(0.f, 0.f, Morale);
				const FColor  BarCol  = (Morale > 60.f) ? FColor::Green
				                      : (Morale > 30.f) ? FColor::Yellow
				                                        : FColor::Red;
				DrawDebugLine(World, BarBase, BarTop, BarCol, false, -1.f, 0, 3.f);
			}

			// Enemy indicator: red ring on the ground
			if (bEnemy && bAlive)
			{
				DrawDebugCircle(World, Pos + FVector(0.f, 0.f, 3.f),
					60.f, 16, FColor::Red, false, -1.f, 0, 3.f,
					FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
			}
		}
	});

	// ── NCOs (podoficerowie) ────────────────────────────────────────────────
	NCODebugQuery.ForEachEntityChunk(Context, [World](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto Morales    = Ctx.GetFragmentView<FMoraleFragment>();
		const auto NCOData    = Ctx.GetFragmentView<FNCOFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			const FVector Pos    = Transforms[i].GetTransform().GetLocation();
			const FVector Up     = FVector(0.f, 0.f, 90.f);
			const float   Morale = Morales[i].Morale;
			const bool    bAlive = NCOData[i].bIsAlive;
			const bool    bEnemy = Factions[i].TeamId != 0;

			// NCO capsule: green for player, dark orange for enemy
			const FColor AliveColor = bEnemy ? FColor(200, 120, 0) : FColor(0, 200, 50);
			const FColor DeadColor  = FColor(80, 80, 80);
			DrawDebugCapsule(World, Pos + Up, 70.f, 30.f, FQuat::Identity,
				bAlive ? AliveColor : DeadColor, false, -1.f, 0, 2.5f);

			// Morale bar
			if (bAlive)
			{
				const FVector BarBase = Pos + FVector(0.f, 0.f, 170.f);
				const FVector BarTop  = BarBase + FVector(0.f, 0.f, Morale);
				const FColor  BarCol  = (Morale > 60.f) ? FColor::Green
				                      : (Morale > 30.f) ? FColor::Yellow
				                                        : FColor::Red;
				DrawDebugLine(World, BarBase, BarTop, BarCol, false, -1.f, 0, 2.5f);
			}

			// Line to target soldier (when chasing)
			if (bAlive && NCOData[i].bHasTarget)
			{
				DrawDebugLine(World,
					Pos + FVector(0.f, 0.f, 50.f),
					Pos + Transforms[i].GetTransform().GetUnitAxis(EAxis::X) * 200.f
						+ FVector(0.f, 0.f, 50.f),
					FColor(0, 255, 100), false, -1.f, 0, 1.5f);
			}

			// Enemy NCO indicator: orange ring
			if (bEnemy && bAlive)
			{
				DrawDebugCircle(World, Pos + FVector(0.f, 0.f, 3.f),
					50.f, 12, FColor(200, 120, 0), false, -1.f, 0, 2.5f,
					FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
			}
		}
	});

	// ── Drummers / fifers (musicians beside the officer) — PURPLE capsule ────
	DrummerDebugQuery.ForEachEntityChunk(Context, [World](FMassExecutionContext& Ctx)
	{
		const auto Transforms  = Ctx.GetFragmentView<FTransformFragment>();
		const auto DrummerData = Ctx.GetFragmentView<FDrummerFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			if (!DrummerData[i].bIsAlive) continue;

			const FVector Pos = Transforms[i].GetTransform().GetLocation();
			// Purple capsule (~175cm) — identifies the musicians at a glance.
			DrawDebugCapsule(World, Pos + FVector(0.f, 0.f, 87.f), 87.f, 25.f,
				FQuat::Identity, FColor(160, 32, 240), false, -1.f, 0, 2.5f);
		}
	});

	// ── Soldiers ────────────────────────────────────────────────────────────
	DebugQuery.ForEachEntityChunk(Context, [World](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto Morales    = Ctx.GetFragmentView<FMoraleFragment>();
		const auto Velocities = Ctx.GetFragmentView<FAgentVelocityFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			const FVector Pos    = Transforms[i].GetTransform().GetLocation();
			const FVector Up     = FVector(0.f, 0.f, 75.f);
			const FColor  Color  = AgentColor(States[i].State);
			const float   Morale = Morales[i].Morale;
			const bool    bEnemy = Factions[i].TeamId != 0;

			// Capsule: colour = current state
			DrawDebugCapsule(World, Pos + Up, 50.f, 25.f, FQuat::Identity,
				Color, false, -1.f, 0, 2.f);

			// Morale bar: vertical line above capsule, length = morale (0–100cm)
			// Color: green > 60, yellow 30–60, red < 30
			if (States[i].State != EAgentState::DEAD)
			{
				const FVector BarBase = Pos + FVector(0.f, 0.f, 140.f);
				const FVector BarTop  = BarBase + FVector(0.f, 0.f, Morale);
				const FColor BarColor = (Morale > 60.f) ? FColor::Green
				                      : (Morale > 30.f) ? FColor::Yellow
				                                        : FColor::Red;
				DrawDebugLine(World, BarBase, BarTop, BarColor, false, -1.f, 0, 2.f);
			}

			// Velocity arrow
			const FVector Vel = Velocities[i].Velocity;
			if (Vel.SizeSquared() > 1.f)
			{
				DrawDebugDirectionalArrow(World,
					Pos + Up,
					Pos + Up + Vel.GetSafeNormal() * 150.f,
					20.f, FColor::White, false, -1.f, 0, 2.f);
			}

			// Enemy indicator: red ring on the ground
			if (bEnemy && States[i].State != EAgentState::DEAD)
			{
				DrawDebugCircle(World, Pos + FVector(0.f, 0.f, 3.f),
					40.f, 12, FColor::Red, false, -1.f, 0, 2.f,
					FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
			}
		}
	});
}
