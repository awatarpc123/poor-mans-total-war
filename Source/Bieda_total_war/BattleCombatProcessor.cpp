#include "BattleCombatProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSpatialGrid.h"
#include "BattleDebugProcessor.h"
#include "BattleSimControl.h"
#include "BattleStats.h"
#include "DrawDebugHelpers.h"

DECLARE_CYCLE_STAT(TEXT("Combat"), STAT_BiedaCombat, STATGROUP_Bieda);

namespace
{
	struct FCombatSnap
	{
		uint8       TeamId;
		EAgentState State;
	};

	struct FBattleShotEvent
	{
		FMassEntityHandle Target;
		FVector ShooterPos;
		FVector TargetPos;
		bool    bHit;
		float   DirShock = 0.f;
		float   HPDamage = 0.f;
	};

	struct FMeleeHitEvent
	{
		FMassEntityHandle Target;
		float             HPDamage;
		float             MoraleShock;
		int32             XHolds = 0;
		float             Knockback = 0.f;
		FVector           AttackDir;
	};

	constexpr float MeleeRange = 200.f;
	constexpr float RearHitDotThresh  =  0.5f;
	constexpr float FrontHitDotThresh = -0.5f;
	constexpr float FlankHitShock     = 3.f;
	constexpr float RearHitShock      = 5.f;
	constexpr float DirBonusFront = 0.f;
	constexpr float DirBonusFlank = 8.f;
	constexpr float DirBonusRear  = 15.f;
	constexpr float HalfChanceDist = 2900.f;

	// ── Misfires (ETW: misfire_musket_flintlock/matchlock/percussion_cap) ──────
	// ETW's own table lists 0.0 for flintlock/matchlock muskets (only cannons
	// and percussion caps misfire in that data) — using that literal value
	// would make this a no-op for our flintlock-era muskets. Using a small,
	// visible flintlock-typical rate instead so the mechanic actually shows up.
	constexpr float MusketMisfireChance = 0.05f;

	// ── Cover (ETW: missile_cover_factor_none/slight/some/good/excellent =
	// 0/3/8/12/16). No terrain/obstacle system exists yet to assign cover from,
	// so this is proxied from FormationRow (0 = front rank): rear ranks are
	// partially shielded by the men in front of them, a real firing-line
	// effect and the only "cover" concept our simulation currently has data
	// for. ETW's point values are folded into accuracy multipliers here.
	constexpr float CoverMult[] = { 1.00f, 0.90f, 0.75f, 0.60f, 0.45f };

	float CoverMultiplierForRow(int32 Row)
	{
		return CoverMult[FMath::Clamp(Row, 0, 4)];
	}
	constexpr float ProjectileArmorDivisor   = 1.f;
	constexpr float ProjectileDefenseDivisor = 2.f;
	constexpr float MinDamageFraction        = 0.15f;
	constexpr float BaseMusketDamage = 60.f;
	constexpr float BaseMeleeDamage  = 40.f;
	constexpr float CloseMax = 1500.f;
	constexpr float LongMax  = 3500.f;
	constexpr float CloseDmg = 1.0f;
	constexpr float LongDmg  = 0.65f;
	constexpr float FarDmg   = 0.30f;
	constexpr int32 HNtoXH_0 = -6;
	constexpr int32 HNtoXH_1 =  0;
	constexpr int32 HNtoXH_2 =  6;
	constexpr int32 HNtoXH_3 = 12;
	constexpr float KB[] = { 70.f, 90.f, 110.f, 150.f, 200.f };
	constexpr float DmgMult[] = { 0.5f, 0.8f, 1.0f, 1.3f, 1.7f };
	constexpr float BlockRange     = 1200.f;
	constexpr float BlockRangeSq   = BlockRange * BlockRange;
	constexpr float CorridorHalf   = 55.f;
	constexpr float CorridorHalfSq = CorridorHalf * CorridorHalf;
	constexpr float MinForward     = 30.f;

	int32 XHoldsFromHN(float Attack, float Defense)
	{
		const int32 hn = FMath::RoundToInt(Attack - Defense);
		if (hn <= HNtoXH_0) return 0;
		if (hn <= HNtoXH_1) return 1;
		if (hn <= HNtoXH_2) return 2;
		if (hn <= HNtoXH_3) return 3;
		return 4;
	}

	float DmgMultByDist(float D)
	{
		if (D <= CloseMax) return CloseDmg;
		if (D <= LongMax)  return LongDmg;
		return FarDmg;
	}

	float AccuracyCurve(float Base, float Dist)
	{
		if (Dist <= 0.f) return Base;
		return Base * FMath::Pow(0.5f, Dist / HalfChanceDist);
	}

	float CalcDamage(float Base, float Armor, float Dfn, float ArmDiv, float DfnDiv)
	{
		const float Reduction = (Armor / ArmDiv) + (Dfn / DfnDiv);
		return FMath::Max(Base * MinDamageFraction, Base - Reduction);
	}
}

UBattleCombatProcessor::UBattleCombatProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	bRequiresGameThreadExecution = true;

	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleStateProcessor")));
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleOfficerProcessor")));
}

void UBattleCombatProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	CombatQuery.Initialize(EntityManager);
	CombatQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	CombatQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.AddRequirement<FAgentCombatFragment>(EMassFragmentAccess::ReadWrite);
	CombatQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.AddRequirement<FFatigueFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.AddRequirement<FAgentVelocityFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.RegisterWithProcessor(*this);
}

void UBattleCombatProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	if (BattleSimPaused()) return;
	SCOPE_CYCLE_COUNTER(STAT_BiedaCombat);
	UWorld* World = GetWorld();

	// ── Snapshot pass ──────────────────────────────────────────────────────────
	TArray<FCombatSnap>       Snaps;
	TArray<FVector>           Positions, Forwards;
	TArray<FMassEntityHandle> Handles;
	TArray<bool>              ForceRunFlags;
	TArray<float>             ArmorVals, DefenseVals;
	TArray<int32>             FormationRows;

	CombatQuery.ForEachEntityChunk(Context,
		[&](FMassExecutionContext& Ctx)
	{
		const auto Tr  = Ctx.GetFragmentView<FTransformFragment>();
		const auto St  = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto Fa  = Ctx.GetFragmentView<FFactionFragment>();
		const auto Ve  = Ctx.GetFragmentView<FAgentVelocityFragment>();
		const auto Co  = Ctx.GetFragmentView<FAgentCombatFragment>();
		const int32 N  = Ctx.GetNumEntities();

		Snaps.Reserve(Snaps.Num()+N); Positions.Reserve(Positions.Num()+N);
		Forwards.Reserve(Forwards.Num()+N); Handles.Reserve(Handles.Num()+N);
		ForceRunFlags.Reserve(ForceRunFlags.Num()+N);
		ArmorVals.Reserve(ArmorVals.Num()+N); DefenseVals.Reserve(DefenseVals.Num()+N);
		FormationRows.Reserve(FormationRows.Num()+N);

		for (int32 i = 0; i < N; ++i)
		{
			const FTransform& T = Tr[i].GetTransform();
			Positions.Add(T.GetLocation());
			Forwards.Add(T.GetUnitAxis(EAxis::X));
			Snaps.Add({ Fa[i].TeamId, St[i].State });
			Handles.Add(Ctx.GetEntity(i));
			ForceRunFlags.Add(Ve[i].bForceRun);
			ArmorVals.Add(Co[i].MeleeDefense * 0.5f);
			DefenseVals.Add(Co[i].MeleeDefense);
			FormationRows.Add(Ve[i].FormationRow);
		}
	});

	if (Snaps.IsEmpty()) return;

	TMap<FMassEntityHandle, int32> HandleToIdx;
	HandleToIdx.Reserve(Handles.Num());
	for (int32 h = 0; h < Handles.Num(); ++h)
		HandleToIdx.Add(Handles[h], h);

	FBattleSpatialGrid Grid;
	Grid.Build(Positions, 2500.f);

	// ── Combat pass ────────────────────────────────────────────────────────────
	TArray<FBattleShotEvent>  ShotEvents;
	TArray<FMeleeHitEvent>    MeleeEvents;
	int32 GlobalIdx = 0;

	CombatQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		const float DT = Ctx.GetDeltaTimeSeconds() * BattleSimTimeScale();
		auto       Tr  = Ctx.GetMutableFragmentView<FTransformFragment>();
		const auto St  = Ctx.GetFragmentView<FAgentStateFragment>();
		auto       Co  = Ctx.GetMutableFragmentView<FAgentCombatFragment>();
		const auto Fa  = Ctx.GetFragmentView<FFactionFragment>();
		const auto Fg  = Ctx.GetFragmentView<FFatigueFragment>();
		const auto Ve  = Ctx.GetFragmentView<FAgentVelocityFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const EAgentState MyState = St[i].State;
			FAgentCombatFragment& CF  = Co[i];
			const uint8 MyTeam        = Fa[i].TeamId;
			const FVector MyPos       = Positions[GlobalIdx];
			const FVector MyFwd       = Forwards[GlobalIdx];
			const bool bCharging      = (MyState == EAgentState::ADVANCING && Ve[i].bForceRun);
			const bool bUseLaneCheck  = (CF.VolleyMode == EVolleyMode::FreeFire);

			// ── AIMING ──────────────────────────────────────────────────────
			if (MyState == EAgentState::AIMING)
			{
				const FVector MyFwdN  = MyFwd.GetSafeNormal2D();
				const float   CosHalf = FMath::Cos(FMath::DegreesToRadians(CF.VisionHalfAngleDeg));
				TArray<int32, TInlineAllocator<24>> EnemyCand, Friends;

				Grid.ForEachInRadius(MyPos, CF.FireRange, GlobalIdx,
					[&](int32 j, float DSq)
				{
					if (Snaps[j].State == EAgentState::DEAD) return;
					if (Snaps[j].TeamId == MyTeam)
					{
						if (bUseLaneCheck && DSq < BlockRangeSq) Friends.Add(j);
						return;
					}
					const FVector To = (Positions[j]-MyPos).GetSafeNormal2D();
					if (FVector::DotProduct(MyFwdN, To) < CosHalf) return;
					EnemyCand.Add(j);
				});

				auto LaneBlocked = [&](const FVector& TP) -> bool
				{
					if (!bUseLaneCheck) return false;
					const float lx=TP.X-MyPos.X, ly=TP.Y-MyPos.Y;
					const float len = FMath::Sqrt(lx*lx+ly*ly);
					if (len < 1.f) return false;
					const float dx=lx/len, dy=ly/len;
					for (int32 f : Friends)
					{
						const float vx=Positions[f].X-MyPos.X, vy=Positions[f].Y-MyPos.Y;
						const float fwd = vx*dx+vy*dy;
						if (fwd <= MinForward || fwd >= len) continue;
						if ((vx*vx+vy*vy)-fwd*fwd < CorridorHalfSq) return true;
					}
					return false;
				};

				int32 TgtIdx = -1;
				if (CF.bHasAcquiredTarget)
					if (const int32* p = HandleToIdx.Find(CF.TargetEntity))
						TgtIdx = *p;

				if (CF.bHasAcquiredTarget)
				{
					bool Ok = false;
					if (TgtIdx>=0 && Snaps[TgtIdx].State!=EAgentState::DEAD)
					{
						const float DSq = (Positions[TgtIdx]-MyPos).SizeSquared2D();
						if (DSq < FMath::Square(CF.FireRange) && !LaneBlocked(Positions[TgtIdx]))
							Ok = true;
					}
					if (!Ok) { CF.bHasAcquiredTarget = false; TgtIdx = -1; }
				}

				if (!CF.bHasAcquiredTarget)
				{
					const FVector Lat(-MyFwdN.Y, MyFwdN.X, 0.f);
					const float   MyLat = FVector::DotProduct(MyPos, Lat);

					const int32 K = (CF.UnitType==EUnitType::LineInfantry) ? 40 : 20;
					int32 Bi[40]; float Bs[40];
					for (int32 c=0;c<K;++c){Bi[c]=-1;Bs[c]=FLT_MAX;}
					int32 Found=0;

					for (int32 j : EnemyCand)
					{
						if (LaneBlocked(Positions[j])) continue;
						const float ELat = FVector::DotProduct(Positions[j],Lat);
						const float Dist = FMath::Sqrt((Positions[j]-MyPos).SizeSquared2D());
						const float Sc   = FMath::Abs(ELat-MyLat)+0.05f*Dist;
						if (Sc < Bs[K-1])
						{
							int32 p=K-1; while(p>0 && Sc<Bs[p-1]){ Bs[p]=Bs[p-1];Bi[p]=Bi[p-1];--p; }
							Bs[p]=Sc; Bi[p]=j; Found=FMath::Min(Found+1,K);
						}
					}

					if (Found>0)
					{
						const int32 Pick = Bi[FMath::RandRange(0,Found-1)];
						CF.TargetEntity = Handles[Pick];
						CF.bHasAcquiredTarget = true;
						TgtIdx = Pick;
					}
				}

				if (CF.bHasAcquiredTarget && TgtIdx>=0)
				{
					const FVector To = (Positions[TgtIdx]-MyPos).GetSafeNormal2D();
					if (!To.IsNearlyZero())
					{
						FTransform T = Tr[i].GetTransform();
						T.SetRotation(FRotationMatrix::MakeFromX(To).ToQuat());
						Tr[i].GetMutableTransform() = T;
					}
					if (World && BiedaDebugDrawEnabled())
						DrawDebugLine(World, MyPos+FVector(0,0,75), Positions[TgtIdx]+FVector(0,0,75),
							FColor::Orange, false, -1.f, 0, 1.f);
				}
			}
			// ── FIRING: ETW accuracy curve + damage formula ────────────────
			else if (MyState == EAgentState::FIRING && CF.bHasAcquiredTarget)
			{
				FVector TgtPos = MyPos + MyFwd * CF.FireRange;
				int32   TgtIdx = -1;
				if (const int32* p = HandleToIdx.Find(CF.TargetEntity))
				{ TgtIdx = *p; TgtPos = Positions[TgtIdx]; }

				const float Dist = (TgtPos-MyPos).Size2D();
				float HitChance = AccuracyCurve(CF.Accuracy, Dist);

				switch (Fg[i].Level)
				{
				case EFatigueLevel::Exhausted: HitChance*=0.50f; break;
				case EFatigueLevel::VeryTired: HitChance*=0.65f; break;
				case EFatigueLevel::Tired:     HitChance*=0.78f; break;
				case EFatigueLevel::Winded:    HitChance*=0.88f; break;
				case EFatigueLevel::Active:    HitChance*=0.95f; break;
				default: break;
				}

				// WAVERING state penalty (-25% accuracy)
				if (MyState == EAgentState::WAVERING)
					HitChance *= 0.75f;

				// Target's cover (rear-rank proxy — see CoverMultiplierForRow)
				if (TgtIdx >= 0)
					HitChance *= CoverMultiplierForRow(FormationRows[TgtIdx]);

				// Misfire: weapon simply fails to discharge, independent of aim
				const bool bMisfire = FMath::FRand() < MusketMisfireChance;
				const bool bHit = !bMisfire && FMath::FRand() < HitChance;
				float DirShock=0.f, HPDamage=0.f;

				if (bHit && TgtIdx>=0)
				{
					const FVector TgtFwd = Forwards[TgtIdx].GetSafeNormal2D();
					const FVector AtkDir = (TgtPos-MyPos).GetSafeNormal2D();
					const float Dot = FVector::DotProduct(AtkDir, TgtFwd);
					if (Dot > RearHitDotThresh)       DirShock = RearHitShock;
					else if (Dot > FrontHitDotThresh) DirShock = FlankHitShock;

					const float RangeMult = DmgMultByDist(Dist);
					HPDamage = CalcDamage(BaseMusketDamage*RangeMult,
						ArmorVals[TgtIdx], DefenseVals[TgtIdx],
						ProjectileArmorDivisor, ProjectileDefenseDivisor);
				}

				ShotEvents.Add({ CF.TargetEntity, MyPos, TgtPos, bHit, DirShock, HPDamage });
				// Deliberately NOT clearing bHasAcquiredTarget here: keep aiming at
				// the same man through the next reload cycle so damage actually
				// accumulates on him. The AIMING branch above already re-validates
				// (alive / in range / not lane-blocked) every time it runs and
				// re-picks if invalid — that's the only gate needed. Clearing here
				// (and in the catch-all below, which used to run EVERY FRAME during
				// the ~15s LOADING state) forced a fresh random pick from the
				// 40-nearest pool every single volley, scattering hits across many
				// different soldiers instead of concentrating fire — this was why
				// shots/hits were non-zero but kills stayed near zero.
			}
			// Any other state (LOADING, HOLDING, etc.): leave bHasAcquiredTarget as
			// -is; see the comment above the FIRING branch for why it must persist.

			// ── Melee contact detection ─────────────────────────────────────
			if (MyState != EAgentState::DEAD)
			{
				CF.bPrevMeleeContact = CF.bInMeleeContact;
				CF.bInMeleeContact   = false;
				Grid.ForEachInRadius(MyPos, MeleeRange, GlobalIdx, [&](int32 j, float)
				{
					if (Snaps[j].TeamId!=MyTeam && Snaps[j].State!=EAgentState::DEAD)
						CF.bInMeleeContact = true;
				});

				if (!CF.bPrevMeleeContact && CF.bInMeleeContact && (bCharging||ForceRunFlags[GlobalIdx]))
				{
					Grid.ForEachInRadius(MyPos, MeleeRange, GlobalIdx, [&](int32 j, float)
					{
						if (Snaps[j].TeamId!=MyTeam && Snaps[j].State!=EAgentState::DEAD)
							MeleeEvents.Add({ Handles[j], 0.f, 8.f });
					});
				}
			}

			// ── MELEE: HN-based hit resolution (ETW style) ─────────────────
			if (MyState == EAgentState::MELEE)
			{
				CF.MeleeTimer += DT;
				if (CF.MeleeTimer >= 0.8f)
				{
					CF.MeleeTimer = 0.f;
					int32 NearEn=-1; float NearDSq=FMath::Square(MeleeRange);
					Grid.ForEachInRadius(MyPos, MeleeRange, GlobalIdx, [&](int32 j, float DSq2D)
					{
						if (Snaps[j].TeamId!=MyTeam && Snaps[j].State!=EAgentState::DEAD && DSq2D<NearDSq)
						{ NearDSq=DSq2D; NearEn=j; }
					});
					if (NearEn>=0)
					{
						const float AtkVal = CF.MeleeAttack + FMath::FRand()*6.f
							+ (bCharging ? CF.MeleeChargeBonus : 0.f);
						const float DefVal = DefenseVals[NearEn] + FMath::FRand()*6.f;

						const FVector TgtFwd = Forwards[NearEn].GetSafeNormal2D();
						const FVector AtkDir = (Positions[NearEn]-MyPos).GetSafeNormal2D();
						const float Dot = FVector::DotProduct(AtkDir, TgtFwd);
						float DirB = DirBonusFront;
						if (Dot > RearHitDotThresh)       DirB = DirBonusRear;
						else if (Dot > FrontHitDotThresh) DirB = DirBonusFlank;

						const int32 XH = XHoldsFromHN(AtkVal+DirB, DefVal);
						CF.LastKnockback  = KB[FMath::Clamp(XH,0,4)];

						float Dmg = CalcDamage(BaseMeleeDamage*DmgMult[FMath::Clamp(XH,0,4)],
							ArmorVals[NearEn], DefenseVals[NearEn], 2.f, 2.f);

						float DirShk = 0.f;
						if (Dot > RearHitDotThresh)       DirShk = RearHitShock;
						else if (Dot > FrontHitDotThresh) DirShk = FlankHitShock;

						MeleeEvents.Add({ Handles[NearEn], Dmg, DirShk, XH,
							CF.LastKnockback, AtkDir });
					}
				}
			}
		}
	});

	// ── Apply ranged damage ────────────────────────────────────────────────────
	for (const FBattleShotEvent& E : ShotEvents)
	{
		if (E.bHit && EntityManager.IsEntityValid(E.Target))
		{
			auto& TCF = EntityManager.GetFragmentDataChecked<FAgentCombatFragment>(E.Target);
			if (TCF.HP <= 0.f) continue;
			TCF.HP -= E.HPDamage;
			if (TCF.HP <= 0.f)
			{
				auto& TSF = EntityManager.GetFragmentDataChecked<FAgentStateFragment>(E.Target);
				TSF.State = EAgentState::DEAD; TSF.StateTimer = 0.f;
			}
			else if (E.DirShock > 0.f)
			{
				auto& TMF = EntityManager.GetFragmentDataChecked<FMoraleFragment>(E.Target);
				TMF.Morale = FMath::Max(0.f, TMF.Morale - E.DirShock);
			}
		}
		if (World && BiedaDebugDrawEnabled())
		{
			DrawDebugLine(World, E.ShooterPos+FVector(0,0,75), E.TargetPos+FVector(0,0,75),
				E.bHit ? FColor::Red : FColor(255,200,0), false, 0.3f, 0, E.bHit ? 2.5f : 1.f);
		}
	}

	// ── Apply melee damage + knockback ─────────────────────────────────────────
	for (const FMeleeHitEvent& E : MeleeEvents)
	{
		if (!EntityManager.IsEntityValid(E.Target)) continue;
		if (E.HPDamage > 0.f)
		{
			auto& TCF = EntityManager.GetFragmentDataChecked<FAgentCombatFragment>(E.Target);
			if (TCF.HP <= 0.f) continue;
			TCF.HP -= E.HPDamage;
			if (TCF.HP <= 0.f)
			{
				auto& TSF = EntityManager.GetFragmentDataChecked<FAgentStateFragment>(E.Target);
				TSF.State = EAgentState::DEAD; TSF.StateTimer = 0.f;
			}
			if (E.Knockback > 0.f)
			{
				auto& TVF = EntityManager.GetFragmentDataChecked<FAgentVelocityFragment>(E.Target);
				TVF.Velocity += E.AttackDir * E.Knockback * 2.f;
			}
		}
		if (E.MoraleShock > 0.f)
		{
			auto& TMF = EntityManager.GetFragmentDataChecked<FMoraleFragment>(E.Target);
			TMF.Morale = FMath::Max(0.f, TMF.Morale - E.MoraleShock);
		}
	}
}