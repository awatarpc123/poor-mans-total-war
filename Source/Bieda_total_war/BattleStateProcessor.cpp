#include "BattleStateProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSimControl.h"
#include "BattleStats.h"

DECLARE_CYCLE_STAT(TEXT("State"), STAT_BiedaState, STATGROUP_Bieda);

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
	if (BattleSimPaused()) return;
	SCOPE_CYCLE_COUNTER(STAT_BiedaState);
	StateQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Ctx)
	{
		const float DT = Ctx.GetDeltaTimeSeconds() * BattleSimTimeScale();

		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		auto       States     = Ctx.GetMutableFragmentView<FAgentStateFragment>();
		auto       Morales    = Ctx.GetMutableFragmentView<FMoraleFragment>();
		const auto Orders     = Ctx.GetFragmentView<FOrderFragment>();
		auto       Combats    = Ctx.GetMutableFragmentView<FAgentCombatFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			FAgentStateFragment&  SF = States[i];
			FMoraleFragment&      MF = Morales[i];
			const FOrderFragment& OF = Orders[i];
			FAgentCombatFragment& CF = Combats[i];

			if (SF.State == EAgentState::DEAD) continue;

			SF.StateTimer += DT;

			// Morale depleted → force ROUTING
			if (MF.Morale <= 0.f)
			{
				MF.Morale = 0.f;
				if (SF.State != EAgentState::ROUTING)
				{
					SF.State      = EAgentState::ROUTING;
					SF.StateTimer = 0.f;
				}
			}

			switch (SF.State)
			{
			case EAgentState::ADVANCING:
				if (CF.bInMeleeContact)
				{
					SF.State      = EAgentState::MELEE;
					SF.StateTimer = 0.f;
				}
				else if (!OF.bHasTarget)
				{
					SF.State      = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
				else
				{
					const FVector Pos    = Transforms[i].GetTransform().GetLocation();
					const float   DistSq = (OF.TargetPosition - Pos).SizeSquared();
					if (DistSq < FMath::Square(150.f))
					{
						SF.State      = EAgentState::HOLDING;
						SF.StateTimer = 0.f;
					}
				}
				break;

			case EAgentState::HOLDING:
				if (CF.bInMeleeContact)
				{
					SF.State      = EAgentState::MELEE;
					SF.StateTimer = 0.f;
					break;
				}
				if (SF.StateTimer >= 2.f)
				{
					if (CF.bMusketLoaded)
					{
						if (CF.VolleyMode == EVolleyMode::FreeFire)
						{
							SF.State      = EAgentState::AIMING;
							SF.StateTimer = 0.f;
						}
						else
						{
							CF.bVolleyReady = true;
							if (CF.bVolleySignal)
							{
								SF.State         = EAgentState::AIMING;
								SF.StateTimer    = 0.f;
								CF.bVolleyReady  = false;
								CF.bVolleySignal = false;
							}
						}
					}
					else
					{
						SF.State      = EAgentState::LOADING;
						SF.StateTimer = 0.f;
					}
				}
				break;

			// ── WAVERING: soldier is uneasy but still operational.
			// Shoots with -25% accuracy (applied in CombatProcessor's accuracy curve
			// via morale check). Won't advance, but will defend position.
			case EAgentState::WAVERING:
				if (CF.bInMeleeContact)
				{
					SF.State      = EAgentState::MELEE;
					SF.StateTimer = 0.f;
					break;
				}
				if (SF.StateTimer >= 2.f)
				{
					if (CF.bMusketLoaded)
					{
						if (CF.VolleyMode == EVolleyMode::FreeFire)
						{
							SF.State      = EAgentState::AIMING;
							SF.StateTimer = 0.f;
						}
						else
						{
							CF.bVolleyReady = true;
							if (CF.bVolleySignal)
							{
								SF.State         = EAgentState::AIMING;
								SF.StateTimer    = 0.f;
								CF.bVolleyReady  = false;
								CF.bVolleySignal = false;
							}
						}
					}
					else
					{
						SF.State      = EAgentState::LOADING;
						SF.StateTimer = 0.f;
					}
				}
				break;

			case EAgentState::LOADING:
				MF.Morale = FMath::Max(0.f, MF.Morale - CF.MoraleDrainLoading * DT);
				if (SF.StateTimer >= CF.ReloadDuration)
				{
					CF.bMusketLoaded = true;
					if (CF.VolleyMode == EVolleyMode::FreeFire)
					{
						SF.State      = EAgentState::AIMING;
						SF.StateTimer = 0.f;
					}
					else
					{
						CF.bVolleyReady = true;
						if (CF.bVolleySignal)
						{
							SF.State         = EAgentState::AIMING;
							SF.StateTimer    = 0.f;
							CF.bVolleyReady  = false;
							CF.bVolleySignal = false;
						}
					}
				}
				break;

			case EAgentState::AIMING:
				if (SF.StateTimer >= CF.AimDuration && CF.bHasAcquiredTarget)
				{
					SF.State      = EAgentState::FIRING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::FIRING:
				if (SF.StateTimer >= CF.FireDuration)
				{
					CF.bMusketLoaded = false;
					SF.State      = EAgentState::LOADING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::ROUTING:
				MF.Morale = FMath::Min(100.f, MF.Morale + CF.RouteRecoveryRate * DT);
				if (MF.Morale > 35.f)
				{
					SF.State      = EAgentState::RALLYING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::RALLYING:
				MF.Morale = FMath::Min(100.f, MF.Morale + 5.f * DT);
				if (MF.Morale > 50.f)
				{
					const FVector Pos = Transforms[i].GetTransform().GetLocation();
					const bool bFarFromSlot = OF.bHasTarget &&
						(OF.TargetPosition - Pos).SizeSquared() > FMath::Square(150.f);
					SF.State      = bFarFromSlot ? EAgentState::ADVANCING : EAgentState::WAVERING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::SHAKEN:
				// SHAKEN: heavy waver, holds fire (won't enter AIMING/FIRING).
				// Recovers to WAVERING at ShakenRecover threshold.
				if (CF.bInMeleeContact)
				{
					SF.State      = EAgentState::MELEE;
					SF.StateTimer = 0.f;
					break;
				}
				MF.Morale = FMath::Min(100.f, MF.Morale + 2.f * DT);
				if (MF.Morale >= CF.ShakenRecover)
				{
					SF.State      = EAgentState::WAVERING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::MELEE:
				MF.Morale = FMath::Max(0.f, MF.Morale - 2.f * DT);
				if (!CF.bInMeleeContact)
				{
					SF.State      = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
				break;

			case EAgentState::PINNED:
				break;
			}

			// ── Morale breakpoints (multi-tier: HOLDING→WAVERING→SHAKEN→ROUTING) ──
			if (SF.State != EAgentState::DEAD     &&
				SF.State != EAgentState::ROUTING  &&
				SF.State != EAgentState::RALLYING)
			{
				if (MF.Morale < CF.PanicThreshold)
				{
					SF.State      = EAgentState::ROUTING;
					SF.StateTimer = 0.f;
				}
				else if ((SF.State == EAgentState::HOLDING   ||
				          SF.State == EAgentState::WAVERING    ||
				          SF.State == EAgentState::LOADING    ||
				          SF.State == EAgentState::AIMING     ||
				          SF.State == EAgentState::FIRING) &&
				         MF.Morale < CF.ShakenThreshold)
				{
					SF.State      = EAgentState::SHAKEN;
					SF.StateTimer = 0.f;
				}
				else if ((SF.State == EAgentState::HOLDING ||
				          SF.State == EAgentState::LOADING ||
				          SF.State == EAgentState::AIMING  ||
				          SF.State == EAgentState::FIRING) &&
				         MF.Morale < CF.WaverThreshold)
				{
					SF.State      = EAgentState::WAVERING;
					SF.StateTimer = 0.f;
				}
				// Recovery: WAVERING→HOLDING at WaverRecover (hysteresis)
				else if (SF.State == EAgentState::WAVERING && MF.Morale >= CF.WaverRecover)
				{
					SF.State      = EAgentState::HOLDING;
					SF.StateTimer = 0.f;
				}
			}
		}
	});
}