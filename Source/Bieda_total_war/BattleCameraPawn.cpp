#include "BattleCameraPawn.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "DrawDebugHelpers.h"
#include "BattleSpawnerActor.h"
#include "BattleSimControl.h"   // ToggleBattleSimPaused()
#include "BattleManager.h"      // EGamePhase, deploy zones
#include "EngineUtils.h"

ABattleCameraPawn::ABattleCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	if (USpectatorPawnMovement* Move = Cast<USpectatorPawnMovement>(GetMovementComponent()))
	{
		Move->MaxSpeed     = NormalSpeed;
		Move->Acceleration = 4000.f;
		Move->Deceleration = 8000.f;
	}
}

void ABattleCameraPawn::BeginPlay()
{
	Super::BeginPlay();

	if (!BattleIMC) BuildInputSetup();
	ApplyMappingContext();

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetShowMouseCursor(true);
		PC->SetInputMode(FInputModeGameAndUI());
	}

	// ── Position camera above the PLAYER'S deploy zone, not the map centre ────
	// Previously averaged every spawner (both sides), which lands the camera
	// on the map centre — a no-man's-land the player hasn't deployed into yet.
	// Prefer the player's own deploy zone (blue box) if a BattleManager exists;
	// fall back to averaging only the player's (TeamId 0) spawners if not.
	FVector CamTarget = FVector::ZeroVector;
	bool bFoundTarget = false;

	for (TActorIterator<ABattleManager> ItMgr(GetWorld()); ItMgr; ++ItMgr)
	{
		CamTarget = ItMgr->GetDeployZone(ItMgr->PlayerTeamId).GetCenter();
		bFoundTarget = true;
		break;
	}

	if (!bFoundTarget)
	{
		int32 SpawnerCount = 0;
		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
		{
			if (It->TeamId != 0) continue;   // player's own squads only
			CamTarget += It->GetActorLocation();
			++SpawnerCount;
		}
		if (SpawnerCount > 0)
		{
			CamTarget /= static_cast<float>(SpawnerCount);
			bFoundTarget = true;
		}
	}

	if (!bFoundTarget)
	{
		// Last resort: average everything (old behaviour) rather than sit at world origin.
		int32 SpawnerCount = 0;
		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
		{
			CamTarget += It->GetActorLocation();
			++SpawnerCount;
		}
		if (SpawnerCount > 0)
			CamTarget /= static_cast<float>(SpawnerCount);
	}

	SetActorLocation(CamTarget + FVector(0.f, -2000.f, 3000.f));

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(-35.f, 0.f, 0.f));
	}
}

void ABattleCameraPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// ── Draw selection rings for all selected units ──────────────────────────
	if (!bIsDragging)
	{
		for (ABattleSpawnerActor* S : SelectedSpawners)
		{
			if (!S) continue;
			const FColor RingColor = (S == SelectedSpawner) ? FColor::Cyan : FColor(0, 180, 255);
			DrawDebugCircle(GetWorld(), S->GetFormationCenter() + FVector(0.f, 0.f, 5.f),
				600.f, 48, RingColor, false, -1.f, 0, 4.f,
				FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
		}
	}

	// ── LMB box-select rectangle preview ────────────────────────────────────
	if (bLMBHeld)
	{
		FVector CursorPos;
		if (GetGroundHitUnderCursor(CursorPos))
		{
			const float DragDist = (CursorPos - DragStartPos).Size2D();
			if (DragDist > DragThreshold)
			{
				bIsDragging = true;
				const float Z = 15.f;
				const FVector CornerA(DragStartPos.X, DragStartPos.Y, Z);
				const FVector CornerB(CursorPos.X,    DragStartPos.Y, Z);
				const FVector CornerC(CursorPos.X,    CursorPos.Y,    Z);
				const FVector CornerD(DragStartPos.X, CursorPos.Y,    Z);
				DrawDebugLine(GetWorld(), CornerA, CornerB, FColor::White, false, -1.f, 0, 2.f);
				DrawDebugLine(GetWorld(), CornerB, CornerC, FColor::White, false, -1.f, 0, 2.f);
				DrawDebugLine(GetWorld(), CornerC, CornerD, FColor::White, false, -1.f, 0, 2.f);
				DrawDebugLine(GetWorld(), CornerD, CornerA, FColor::White, false, -1.f, 0, 2.f);
			}
		}
	}

	// ── RMB drag-to-form preview ─────────────────────────────────────────────
	if (bRMBHeld && !SelectedSpawners.IsEmpty())
	{
		FVector CursorPos;
		if (GetGroundHitUnderCursor(CursorPos))
		{
			const float DragDist = (CursorPos - RMBDragStart).Size2D();
			if (DragDist > DragThreshold)
			{
				bRMBDragging = true;
				const FVector DragDir  = (CursorPos - RMBDragStart).GetSafeNormal2D();
				const FVector FrontDir = -DragDir;

				DrawDebugLine(GetWorld(),
					RMBDragStart + FVector(0.f, 0.f, 10.f),
					CursorPos    + FVector(0.f, 0.f, 10.f),
					FColor::Green, false, -1.f, 0, 3.f);

				if (SelectedSpawners.Num() == 1 && SelectedSpawner)
				{
					const int32 RowSize = FMath::Clamp(
						FMath::RoundToInt(DragDist / SelectedSpawner->SpawnSpacing),
						1, FMath::Max(1, SelectedSpawner->CountLiving()));
					DrawFormationPreview((RMBDragStart + CursorPos) * 0.5f, RowSize,
						SelectedSpawner->CountLiving(), SelectedSpawner->SpawnSpacing, FrontDir);
				}
				else
				{
					// Multiple units: show each squad distributed along the drag line.
					int32 TotalLiving = 0;
					for (ABattleSpawnerActor* S : SelectedSpawners) if (S) TotalLiving += S->CountLiving();
					if (TotalLiving > 0)
					{
						float SegOffset = 0.f;
						for (ABattleSpawnerActor* S : SelectedSpawners)
						{
							if (!S || S->CountLiving() <= 0) continue;
							const float Frac  = (float)S->CountLiving() / TotalLiving;
							const float Width = DragDist * Frac;
							const int32 RowSize = FMath::Max(1, FMath::RoundToInt(Width / S->SpawnSpacing));
							const FVector SegCenter = RMBDragStart + DragDir * (SegOffset + Width * 0.5f);
							DrawFormationPreview(SegCenter, RowSize, S->CountLiving(), S->SpawnSpacing, FrontDir);
							SegOffset += Width;
						}
					}
				}
			}
		}
	}
}

void ABattleCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// NOTE: NOT calling Super — ADefaultPawn binds mouse axes for rotation
	// which causes LMB to rotate camera. We handle everything via Enhanced Input.

	if (!BattleIMC) BuildInputSetup();

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EIC) return;

	EIC->BindAction(MoveForwardAction, ETriggerEvent::Triggered, this, &ABattleCameraPawn::OnMoveForward);
	EIC->BindAction(MoveRightAction,   ETriggerEvent::Triggered, this, &ABattleCameraPawn::OnMoveRight);
	EIC->BindAction(MoveUpAction,      ETriggerEvent::Triggered, this, &ABattleCameraPawn::OnMoveUp);
	EIC->BindAction(LookAction,        ETriggerEvent::Triggered, this, &ABattleCameraPawn::OnLook);
	EIC->BindAction(SpeedBoostAction,  ETriggerEvent::Started,   this, &ABattleCameraPawn::OnSpeedBoostStart);
	EIC->BindAction(SpeedBoostAction,  ETriggerEvent::Completed, this, &ABattleCameraPawn::OnSpeedBoostEnd);

	// LMB: Started = press, Completed = release
	EIC->BindAction(LMBAction,         ETriggerEvent::Started,   this, &ABattleCameraPawn::OnLMBDown);
	EIC->BindAction(LMBAction,         ETriggerEvent::Completed, this, &ABattleCameraPawn::OnLMBUp);

	EIC->BindAction(RMBAction,         ETriggerEvent::Started,   this, &ABattleCameraPawn::OnRMBStart);
	EIC->BindAction(RMBAction,         ETriggerEvent::Completed, this, &ABattleCameraPawn::OnRMBEnd);
	EIC->BindAction(MMBAction,         ETriggerEvent::Started,   this, &ABattleCameraPawn::OnMMBStart);
	EIC->BindAction(MMBAction,         ETriggerEvent::Completed, this, &ABattleCameraPawn::OnMMBEnd);
	EIC->BindAction(ESCAction,         ETriggerEvent::Started,   this, &ABattleCameraPawn::OnESCPress);
	EIC->BindAction(PauseAction,       ETriggerEvent::Started,   this, &ABattleCameraPawn::OnPausePress);
	EIC->BindAction(SlowerAction,      ETriggerEvent::Started,   this, &ABattleCameraPawn::OnSlowerPress);
	EIC->BindAction(FasterAction,      ETriggerEvent::Started,   this, &ABattleCameraPawn::OnFasterPress);

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetShowMouseCursor(true);
		PC->SetInputMode(FInputModeGameAndUI());
	}
}

void ABattleCameraPawn::BuildInputSetup()
{
	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type) -> UInputAction*
	{
		UInputAction* A = NewObject<UInputAction>(this, Name);
		A->ValueType = Type;
		return A;
	};

	MoveForwardAction = MakeAction(TEXT("BattleMoveForward"), EInputActionValueType::Axis1D);
	MoveRightAction   = MakeAction(TEXT("BattleMoveRight"),   EInputActionValueType::Axis1D);
	MoveUpAction      = MakeAction(TEXT("BattleMoveUp"),      EInputActionValueType::Axis1D);
	LookAction        = MakeAction(TEXT("BattleLook"),        EInputActionValueType::Axis2D);
	SpeedBoostAction  = MakeAction(TEXT("BattleSpeedBoost"),  EInputActionValueType::Boolean);
	LMBAction         = MakeAction(TEXT("BattleLMB"),         EInputActionValueType::Boolean);
	LMBAction->Triggers.Add(NewObject<UInputTriggerDown>(this));

	RMBAction         = MakeAction(TEXT("BattleRMB"),         EInputActionValueType::Boolean);
	RMBAction->Triggers.Add(NewObject<UInputTriggerDown>(this));
	MMBAction         = MakeAction(TEXT("BattleMMB"),         EInputActionValueType::Boolean);
	MMBAction->Triggers.Add(NewObject<UInputTriggerDown>(this));
	ESCAction         = MakeAction(TEXT("BattleESC"),         EInputActionValueType::Boolean);
	PauseAction       = MakeAction(TEXT("BattlePause"),       EInputActionValueType::Boolean);
	SlowerAction      = MakeAction(TEXT("BattleSlower"),      EInputActionValueType::Boolean);
	FasterAction      = MakeAction(TEXT("BattleFaster"),      EInputActionValueType::Boolean);

	BattleIMC = NewObject<UInputMappingContext>(this, TEXT("BattleIMC"));

	auto AddNegKey = [this](UInputAction* Action, FKey Key)
	{
		FEnhancedActionKeyMapping& M = BattleIMC->MapKey(Action, Key);
		UInputModifierNegate* Neg = NewObject<UInputModifierNegate>(this);
		M.Modifiers.Add(Neg);
	};

	BattleIMC->MapKey(MoveForwardAction, EKeys::W);
	AddNegKey(MoveForwardAction, EKeys::S);
	AddNegKey(MoveRightAction,   EKeys::A);
	BattleIMC->MapKey(MoveRightAction,   EKeys::D);
	BattleIMC->MapKey(MoveUpAction,      EKeys::E);
	AddNegKey(MoveUpAction,      EKeys::Q);

	BattleIMC->MapKey(LookAction,        EKeys::Mouse2D);
	BattleIMC->MapKey(SpeedBoostAction,  EKeys::LeftShift);

	BattleIMC->MapKey(LMBAction,         EKeys::LeftMouseButton);
	BattleIMC->MapKey(RMBAction,         EKeys::RightMouseButton);
	BattleIMC->MapKey(MMBAction,         EKeys::MiddleMouseButton);
	BattleIMC->MapKey(ESCAction,         EKeys::Escape);
	BattleIMC->MapKey(PauseAction,       EKeys::SpaceBar);
	BattleIMC->MapKey(SlowerAction,      EKeys::LeftBracket);
	BattleIMC->MapKey(FasterAction,      EKeys::RightBracket);
}

void ABattleCameraPawn::ApplyMappingContext()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return;

	UEnhancedInputLocalPlayerSubsystem* Sub =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
	if (Sub) Sub->AddMappingContext(BattleIMC, 0);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

bool ABattleCameraPawn::GetGroundHitUnderCursor(FVector& OutHit) const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return false;

	FVector WorldOrigin, WorldDir;
	if (!PC->DeprojectMousePositionToWorld(WorldOrigin, WorldDir)) return false;
	if (FMath::Abs(WorldDir.Z) < KINDA_SMALL_NUMBER) return false;

	const float T = -WorldOrigin.Z / WorldDir.Z;
	if (T <= 0.f) return false;

	OutHit = WorldOrigin + WorldDir * T;
	return true;
}

bool ABattleCameraPawn::IsCursorOverUI() const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return false;

	float MouseX, MouseY;
	PC->GetMousePosition(MouseX, MouseY);

	int32 ViewX, ViewY;
	PC->GetViewportSize(ViewX, ViewY);

	if (ViewY <= 0) return false;

	// UI panel occupies roughly the bottom 15% of the screen
	return MouseY > ViewY * 0.84f;
}

void ABattleCameraPawn::DrawFormationPreview(const FVector& Center, int32 RowSize,
	int32 NumAgents, float Spacing, const FVector& FrontLineDir)
{
	UWorld* World = GetWorld();
	if (!World) return;

	const FQuat FormRot   = FRotationMatrix::MakeFromX(FrontLineDir).ToQuat();
	const int32 NumRows   = FMath::Max(1, FMath::CeilToInt((float)NumAgents / RowSize));
	const float HalfFront = RowSize * Spacing * 0.5f;
	const float HalfDepth = NumRows * Spacing * 0.5f;

	// Draw each soldier slot as a yellow dot
	for (int32 i = 0; i < NumAgents; ++i)
	{
		const int32 Col = i % RowSize;
		const int32 Row = i / RowSize;
		const int32 InRow = FMath::Min(RowSize, NumAgents - Row * RowSize);
		const float CenterShift = (RowSize - InRow) * Spacing * 0.5f;  // centre partial last row
		const FVector LocalOffset(
			Col * Spacing - HalfFront + CenterShift,
			HalfDepth - Row * Spacing,
			0.f
		);
		const FVector SlotPos = Center + FormRot.RotateVector(LocalOffset);

		DrawDebugPoint(World, SlotPos + FVector(0.f, 0.f, 10.f),
			10.f, FColor::Yellow, false, -1.f, 0);
	}

	// Draw front line (row 0) as a green line
	const FVector FrontLeft  = Center + FormRot.RotateVector(FVector(-HalfFront, HalfDepth, 0.f));
	const FVector FrontRight = Center + FormRot.RotateVector(FVector(
		(RowSize - 1) * Spacing - HalfFront, HalfDepth, 0.f));
	DrawDebugLine(World,
		FrontLeft + FVector(0.f, 0.f, 10.f),
		FrontRight + FVector(0.f, 0.f, 10.f),
		FColor::Green, false, -1.f, 0, 4.f);

	// Draw formation center
	DrawDebugCircle(World, Center + FVector(0.f, 0.f, 5.f),
		100.f, 16, FColor::Green, false, -1.f, 0, 3.f,
		FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
}

// ── Movement ─────────────────────────────────────────────────────────────────

void ABattleCameraPawn::OnMoveForward(const FInputActionValue& Value)
{
	const FRotator YawRot(0.f, GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::X), Value.Get<float>());
}

void ABattleCameraPawn::OnMoveRight(const FInputActionValue& Value)
{
	const FRotator YawRot(0.f, GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), Value.Get<float>());
}

void ABattleCameraPawn::OnMoveUp(const FInputActionValue& Value)
{
	AddMovementInput(FVector::UpVector, Value.Get<float>());
}

void ABattleCameraPawn::OnLook(const FInputActionValue& Value)
{
	if (!bMMBHeld) return;
	const FVector2D Delta = Value.Get<FVector2D>();

	AddControllerYawInput(Delta.X);

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		FRotator Rot = PC->GetControlRotation();
		Rot.Pitch = FMath::ClampAngle(Rot.Pitch + Delta.Y, -80.f, 80.f);
		PC->SetControlRotation(Rot);
	}
}

void ABattleCameraPawn::OnSpeedBoostStart(const FInputActionValue& Value)
{
	if (USpectatorPawnMovement* Move = Cast<USpectatorPawnMovement>(GetMovementComponent()))
		Move->MaxSpeed = TurboSpeed;
}

void ABattleCameraPawn::OnSpeedBoostEnd(const FInputActionValue& Value)
{
	if (USpectatorPawnMovement* Move = Cast<USpectatorPawnMovement>(GetMovementComponent()))
		Move->MaxSpeed = NormalSpeed;
}

// ── MMB: hold to rotate camera ───────────────────────────────────────────────

void ABattleCameraPawn::OnMMBStart(const FInputActionValue& Value)
{
	bMMBHeld = true;
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
		PC->SetShowMouseCursor(false);
}

void ABattleCameraPawn::OnMMBEnd(const FInputActionValue& Value)
{
	bMMBHeld = false;
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
		PC->SetShowMouseCursor(true);
}

// ── RMB: issue orders (press = record, release = act) ────────────────────────

void ABattleCameraPawn::OnRMBStart(const FInputActionValue& Value)
{
	if (bMMBHeld) return;
	if (IsCursorOverUI()) return;

	bRMBHeld     = true;
	bRMBDragging = false;
	if (!GetGroundHitUnderCursor(RMBDragStart)) return;

	// Pressed on empty ground → halt all selected units now; the real move order
	// comes on release so they don't run a stale order while we drag-to-form.
	if (!SelectedSpawners.IsEmpty())
	{
		bool bOnUnit = false;
		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
			if (It->GetClosestSoldierDistSq(RMBDragStart) < FMath::Square(SelectionRadius))
			{ bOnUnit = true; break; }
		if (!bOnUnit)
			for (ABattleSpawnerActor* S : SelectedSpawners) if (S) S->HoldPosition();
	}
}

void ABattleCameraPawn::OnRMBEnd(const FInputActionValue& Value)
{
	if (!bRMBHeld) { return; }
	bRMBHeld = false;
	if (IsCursorOverUI() || SelectedSpawners.IsEmpty()) { bRMBDragging = false; return; }

	FVector CursorPos;
	if (!GetGroundHitUnderCursor(CursorPos)) { bRMBDragging = false; return; }

	const float DragDist = (CursorPos - RMBDragStart).Size2D();

	// Resolve deploy phase once.
	bool bDeploy = false;
	FBox DeployZone;
	for (TActorIterator<ABattleManager> It(GetWorld()); It; ++It)
	{
		if (It->GetGamePhase() == EGamePhase::Deploy) { bDeploy = true; DeployZone = It->GetDeployZone(0); }
		break;
	}
	auto ClampDeploy = [&](FVector V) -> FVector {
		if (bDeploy) { V.X = FMath::Clamp(V.X, DeployZone.Min.X, DeployZone.Max.X);
		               V.Y = FMath::Clamp(V.Y, DeployZone.Min.Y, DeployZone.Max.Y); }
		return V;
	};

	// ── DRAG: form units along the drag line ─────────────────────────────────
	if (DragDist > DragThreshold)
	{
		const FVector DragDir  = (CursorPos - RMBDragStart).GetSafeNormal2D();
		const FVector FrontDir = -DragDir;

		if (SelectedSpawners.Num() == 1 && SelectedSpawner)
		{
			const int32 RowSize = FMath::Clamp(
				FMath::RoundToInt(DragDist / SelectedSpawner->SpawnSpacing),
				1, FMath::Max(1, SelectedSpawner->CountLiving()));
			SelectedSpawner->IssueMoveOrder(
				ClampDeploy((RMBDragStart + CursorPos) * 0.5f), RowSize, FrontDir, bDeploy);
		}
		else
		{
			// Shared front: distribute drag line proportionally by living count.
			int32 TotalLiving = 0;
			for (ABattleSpawnerActor* S : SelectedSpawners) if (S) TotalLiving += S->CountLiving();
			if (TotalLiving > 0)
			{
				float SegOffset = 0.f;
				for (ABattleSpawnerActor* S : SelectedSpawners)
				{
					if (!S || S->CountLiving() <= 0) continue;
					const float Frac    = (float)S->CountLiving() / TotalLiving;
					const float Width   = DragDist * Frac;
					const int32 RowSize = FMath::Max(1, FMath::RoundToInt(Width / S->SpawnSpacing));
					const FVector Seg   = RMBDragStart + DragDir * (SegOffset + Width * 0.5f);
					S->IssueMoveOrder(ClampDeploy(Seg), RowSize, FrontDir, bDeploy);
					SegOffset += Width;
				}
			}
		}
		bRMBDragging = false;
		return;
	}

	bRMBDragging = false;

	// ── CLICK in deploy: teleport primary unit to cursor ──────────────────────
	if (bDeploy)
	{
		if (SelectedSpawner)
			SelectedSpawner->IssueMoveOrder(ClampDeploy(CursorPos),
				SelectedSpawner->GetCurrentRowSize(), FVector::ZeroVector, /*bInstant*/ true);
		return;
	}

	// ── CLICK in battle: near enemy → engage all; otherwise group quick-move ──
	ABattleSpawnerActor* BestEnemy = nullptr;
	float BestEnemyDistSq = FMath::Square(SelectionRadius);
	for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
	{
		if (It->TeamId == 0) continue;
		const float DistSq = It->GetClosestSoldierDistSq(CursorPos);
		if (DistSq < BestEnemyDistSq) { BestEnemyDistSq = DistSq; BestEnemy = *It; }
	}
	if (BestEnemy)
	{
		for (ABattleSpawnerActor* S : SelectedSpawners) if (S) S->IssueEngageOrder(BestEnemy);
		return;
	}

	// Quick-move: preserve each squad's relative offset from the group centre.
	FVector GroupCenter = FVector::ZeroVector;
	int32 Count = 0;
	for (ABattleSpawnerActor* S : SelectedSpawners) if (S) { GroupCenter += S->GetFormationCenter(); ++Count; }
	if (Count > 0) GroupCenter /= (float)Count;

	for (ABattleSpawnerActor* S : SelectedSpawners)
	{
		if (!S) continue;
		const FVector Offset = S->GetFormationCenter() - GroupCenter;
		S->IssueMoveOrder(CursorPos + Offset, S->GetCurrentRowSize());
	}
}

// ── ESC: deselect ────────────────────────────────────────────────────────────

void ABattleCameraPawn::OnESCPress(const FInputActionValue& Value)
{
	SelectedSpawner = nullptr;
	SelectedSpawners.Reset();
}

void ABattleCameraPawn::OnPausePress(const FInputActionValue& Value)
{
	// Tactical pause: freezes the battle simulation only. This pawn's Tick
	// (camera) and the order handlers keep running, so the player can pan and
	// queue move/attack orders while time is stopped — Total War style.
	ToggleBattleSimPaused();
}

void ABattleCameraPawn::OnSlowerPress(const FInputActionValue& Value)
{
	StepBattleSimTimeScale(-1);   // [ → slow down the battle
}

void ABattleCameraPawn::OnFasterPress(const FInputActionValue& Value)
{
	StepBattleSimTimeScale(+1);   // ] → speed up the battle
}

// ── LMB: press → record corner, release → box-select or single-select ────────

void ABattleCameraPawn::OnLMBDown(const FInputActionValue& Value)
{
	if (bMMBHeld) return;
	if (IsCursorOverUI()) return;

	bLMBHeld    = true;
	bIsDragging = false;
	GetGroundHitUnderCursor(DragStartPos);
}

void ABattleCameraPawn::OnLMBUp(const FInputActionValue& Value)
{
	if (!bLMBHeld) { return; }
	bLMBHeld = false;
	if (IsCursorOverUI()) { bIsDragging = false; return; }

	FVector CursorPos;
	if (!GetGroundHitUnderCursor(CursorPos)) { bIsDragging = false; return; }

	if (bIsDragging)
	{
		BoxSelect(DragStartPos, CursorPos);
	}
	else
	{
		// Single click: select nearest player squad within SelectionRadius.
		ABattleSpawnerActor* Best = nullptr;
		float BestDistSq = FMath::Square(SelectionRadius);
		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
		{
			if (It->TeamId != 0) continue;
			const float D = It->GetClosestSoldierDistSq(CursorPos);
			if (D < BestDistSq) { BestDistSq = D; Best = *It; }
		}
		if (Best)
			SetSelectedSpawner(Best);
		else
		{
			SelectedSpawner = nullptr;
			SelectedSpawners.Reset();
		}
	}
	bIsDragging = false;
}

void ABattleCameraPawn::BoxSelect(const FVector& GroundA, const FVector& GroundB)
{
	const float MinX = FMath::Min(GroundA.X, GroundB.X);
	const float MaxX = FMath::Max(GroundA.X, GroundB.X);
	const float MinY = FMath::Min(GroundA.Y, GroundB.Y);
	const float MaxY = FMath::Max(GroundA.Y, GroundB.Y);

	SelectedSpawner = nullptr;
	SelectedSpawners.Reset();

	for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
	{
		if (It->TeamId != 0) continue;
		const FVector Centre = It->GetFormationCenter();
		if (Centre.X >= MinX && Centre.X <= MaxX && Centre.Y >= MinY && Centre.Y <= MaxY)
		{
			if (!SelectedSpawner) SelectedSpawner = *It;
			SelectedSpawners.Add(*It);
		}
	}
}

// F1-F5 commands removed — now handled by clickable UI buttons in SBattleHUDWidget
