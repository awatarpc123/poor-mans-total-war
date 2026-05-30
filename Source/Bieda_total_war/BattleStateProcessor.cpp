#include "BattleStateProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"

UBattleStateProcessor::UBattleStateProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleMovementProcessor")));
}

void UBattleStateProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	StateQuery.Initialize(EntityManager);
	StateQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	StateQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadWrite);
	StateQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadWrite);
	StateQuery.AddRequirement<FOrderFragment>(EMassFragmentAccess::ReadOnly);
	StateQuery.AddRequirement<FAgentCombatFragment>(EMassFragmentAccess::ReadWrite);
	StateQuery.RegisterWithProcessor(*this);
}

void UBattleStateProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	StateQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Ctx)
	{
		const float DT = Ctx.GetDeltaTimeSeconds();

		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		auto       States     = Ctx.GetMutableFragmentView<FAgentStateFragment>();
		auto       Morales    = Ctx.GetMutableFragmentView<FMoraleFragment>();
		const auto Orders     = Ctx.GetFragmentView<FOrderFragment>();
		auto       Combats    = Ctx.GetMutableFragmentView<FAgentCombatFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			FAgentStateFragment&        SF = States[i];
			FMoraleFragment&            MF = Morales[i];
			const FOrderFragment&       OF = Orders[i];
			FAgentCombatFragment& CF = Combats[i];

			if (SF.State == EAgentState::DEAD) continue;

			SF.StateTimer += DT;

			// Morale depleted → force ROUTING (not DEAD).
			// Death is reserved for future combat damage (bullets, melee).
			if (MF.Morale <= 0.f)
			{
				MF.Morale = 0.f;
				if (SF.State != EAgentState::ROUTING)
				{
					SF.State      = EAgentState::ROUTING;
					SF.StateTimer = 0.f;
				}
				// fall through — let ROUTING recovery run below
			}

			switch (SF.State)
			{
			case EAgentState::ADVANCING:
				if (!OF.bHasTarget)
				{
					SF.State     = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
				else
				{
					const FVector Pos    = Transforms[i].GetTransform().GetLocation();
					const float   DistSq = (OF.TargetPosition - Pos).SizeSquared();
					if (DistSq < FMath::Square(150.f))
					{
						SF.State     = EAgentState::HOLDING;
						SF.StateTimer = 0.f;
					}
				}
				break;

			case EAgentState::HOLDING:
				if (SF.StateTimer >= 2.f)
				{
					SF.State     = EAgentState::LOADING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::LOADING:
				MF.Morale = FMath::Max(0.f, MF.Morale - CF.MoraleDrainLoading * DT);
				if (SF.StateTimer >= CF.ReloadDuration)
				{
					if (CF.VolleyMode == EVolleyMode::FreeFire)
					{
						// Militia / free fire: transition immediately
						SF.State      = EAgentState::AIMING;
						SF.StateTimer = 0.f;
					}
					else
					{
						// Volley mode: mark ready, wait for coordinator signal
						CF.bVolleyReady = true;
						if (CF.bVolleySignal)
						{
							SF.State         = EAgentState::AIMING;
							SF.StateTimer    = 0.f;
							CF.bVolleyReady  = false;
							CF.bVolleySignal = false;
						}
						// else: stay in LOADING, musket loaded, waiting
					}
				}
				break;

			case EAgentState::AIMING:
				// Transition only when aim timer is done AND a target is locked.
				// CombatProcessor (runs after us) sets bHasAcquiredTarget.
				if (SF.StateTimer >= CF.AimDuration && CF.bHasAcquiredTarget)
				{
					SF.State     = EAgentState::FIRING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::FIRING:
				if (SF.StateTimer >= CF.FireDuration)
				{
					// Self-fire morale drain removed — firing your own musket
					// shouldn't break morale.  Enemy fire does that via ThreatActors.
					SF.State     = EAgentState::LOADING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::ROUTING:
				MF.Morale = FMath::Min(100.f, MF.Morale + CF.RouteRecoveryRate * DT);
				if (MF.Morale > 35.f)
				{
					SF.State     = EAgentState::RALLYING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::RALLYING:
				MF.Morale = FMath::Min(100.f, MF.Morale + 5.f * DT);
				if (MF.Morale > 50.f)
				{
					SF.State     = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::SHAKEN:
			{
				// Wavering: hold position, hold fire (won't enter AIMING/FIRING).
				// Steady slowly on your own; an officer aura or an NCO rallying
				// nearby pulls morale up much faster (their processors do that).
				// Climb past ShakenRecover → back into the firing line.
				constexpr float ShakenSteadyRate = 1.5f;   // morale/s passive recovery
				MF.Morale = FMath::Min(100.f, MF.Morale + ShakenSteadyRate * DT);
				if (MF.Morale >= CF.ShakenRecover)
				{
					SF.State      = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
				break;
			}

			case EAgentState::MELEE:
			case EAgentState::PINNED:
				break;
			}

			// ── Morale breakpoints after per-state logic (two-tier) ────────
			// Tier 1: morale < PanicThreshold      → ROUTING  (full panic, run)
			// Tier 2: morale < ShakenThreshold     → SHAKEN   (wavering, hold fire)
			// SHAKEN only from a stationary combat stance; advancing/melee/pinned
			// soldiers don't freeze. SHAKEN itself can still fall through to ROUTING.
			if (SF.State != EAgentState::DEAD     &&
				SF.State != EAgentState::ROUTING  &&
				SF.State != EAgentState::RALLYING)
			{
				if (MF.Morale < CF.PanicThreshold)
				{
					SF.State      = EAgentState::ROUTING;
					SF.StateTimer = 0.f;
				}
				else if ((SF.State == EAgentState::HOLDING ||
				          SF.State == EAgentState::LOADING ||
				          SF.State == EAgentState::AIMING  ||
				          SF.State == EAgentState::FIRING) &&
				         MF.Morale < CF.ShakenThreshold)
				{
					SF.State      = EAgentState::SHAKEN;
					SF.StateTimer = 0.f;
				}
			}
		}
	});
}
