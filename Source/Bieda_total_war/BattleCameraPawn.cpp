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

	// ── Position camera above the battlefield ────────────────────────────────
	FVector CamTarget = FVector::ZeroVector;
	int32 SpawnerCount = 0;
	for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
	{
		CamTarget += It->GetActorLocation();
		++SpawnerCount;
	}
	if (SpawnerCount > 0)
		CamTarget /= static_cast<float>(SpawnerCount);

	SetActorLocation(CamTarget + FVector(0.f, -2000.f, 3000.f));

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(-35.f, 0.f, 0.f));
	}
}

void ABattleCameraPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// ── Draw selection ring ──────────────────────────────────────────────────
	if (SelectedSpawner && !bIsDragging)
	{
		const FVector Center = SelectedSpawner->GetFormationCenter();
		DrawDebugCircle(GetWorld(), Center + FVector(0.f, 0.f, 5.f),
			600.f, 48, FColor::Cyan, false, -1.f, 0, 4.f,
			FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
	}

	// ── Drag-to-form preview ─────────────────────────────────────────────────
	if (bLMBHeld && SelectedSpawner)
	{
		FVector CursorPos;
		if (GetGroundHitUnderCursor(CursorPos))
		{
			const float DragDist = (CursorPos - DragStartPos).Size2D();

			if (DragDist > DragThreshold)
			{
				bIsDragging = true;

				const FVector DragDir    = (CursorPos - DragStartPos).GetSafeNormal2D();
				const FVector LineCenter = (DragStartPos + CursorPos) * 0.5f;

				const int32 RowSize = FMath::Clamp(
					FMath::RoundToInt(DragDist / SelectedSpawner->SpawnSpacing),
					3, SelectedSpawner->NumAgents);

				// Draw the drag line itself
				DrawDebugLine(GetWorld(),
					DragStartPos + FVector(0.f, 0.f, 10.f),
					CursorPos + FVector(0.f, 0.f, 10.f),
					FColor::Green, false, -1.f, 0, 3.f);

				// Draw formation preview
				DrawFormationPreview(LineCenter, RowSize,
					SelectedSpawner->NumAgents,
					SelectedSpawner->SpawnSpacing, DragDir);
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
	EIC->BindAction(ESCAction,         ETriggerEvent::Started,   this, &ABattleCameraPawn::OnESCPress);

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
	ESCAction         = MakeAction(TEXT("BattleESC"),         EInputActionValueType::Boolean);

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
	BattleIMC->MapKey(ESCAction,         EKeys::Escape);
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
		const FVector LocalOffset(
			Col * Spacing - HalfFront,
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
	if (!bRMBHeld) return;
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

// ── RMB: hold to rotate camera ───────────────────────────────────────────────

void ABattleCameraPawn::OnRMBStart(const FInputActionValue& Value)
{
	bRMBHeld = true;
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
		PC->SetShowMouseCursor(false);
}

void ABattleCameraPawn::OnRMBEnd(const FInputActionValue& Value)
{
	bRMBHeld = false;
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
		PC->SetShowMouseCursor(true);
}

// ── ESC: deselect ────────────────────────────────────────────────────────────

void ABattleCameraPawn::OnESCPress(const FInputActionValue& Value)
{
	SelectedSpawner = nullptr;
}

// ── LMB: press → record, release → act ──────────────────────────────────────

void ABattleCameraPawn::OnLMBDown(const FInputActionValue& Value)
{
	if (bRMBHeld) return;
	if (IsCursorOverUI()) return;   // let Slate buttons handle it

	bLMBHeld    = true;
	bIsDragging = false;

	if (!GetGroundHitUnderCursor(DragStartPos)) return;

	// If a unit is selected and we clicked on empty ground (not on any formation),
	// halt the unit immediately — the actual order comes on LMBUp (release).
	// This prevents soldiers from executing a previous order while we drag-to-form.
	if (SelectedSpawner)
	{
		bool bClickedOnUnit = false;
		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
		{
			const float DistSq = (It->GetFormationCenter() - DragStartPos).SizeSquared2D();
			if (DistSq < FMath::Square(SelectionRadius))
			{
				bClickedOnUnit = true;
				break;
			}
		}

		if (!bClickedOnUnit)
		{
			SelectedSpawner->HoldPosition();
		}
	}
}

void ABattleCameraPawn::OnLMBUp(const FInputActionValue& Value)
{
	if (!bLMBHeld) { return; }
	bLMBHeld = false;
	if (IsCursorOverUI()) { bIsDragging = false; return; }

	// ── DRAG: issue move order with new formation ────────────────────────────
	if (bIsDragging && SelectedSpawner)
	{
		FVector CursorPos;
		if (GetGroundHitUnderCursor(CursorPos))
		{
			const FVector DragDir    = (CursorPos - DragStartPos).GetSafeNormal2D();
			const float   DragDist  = (CursorPos - DragStartPos).Size2D();
			const FVector LineCenter = (DragStartPos + CursorPos) * 0.5f;

			const int32 RowSize = FMath::Clamp(
				FMath::RoundToInt(DragDist / SelectedSpawner->SpawnSpacing),
				3, SelectedSpawner->NumAgents);

			// Green confirmation circle
			DrawDebugCircle(GetWorld(), LineCenter + FVector(0.f, 0.f, 5.f),
				300.f, 32, FColor::Green, false, 1.5f, 0, 4.f,
				FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));

			SelectedSpawner->IssueMoveOrder(LineCenter, RowSize, DragDir);
		}

		bIsDragging = false;
		return;
	}

	bIsDragging = false;

	// ── SHORT CLICK: select or quick-move ────────────────────────────────────
	FVector ClickPos;
	if (!GetGroundHitUnderCursor(ClickPos)) return;

	// Try to select a formation
	ABattleSpawnerActor* BestSpawner = nullptr;
	float BestDistSq = FMath::Square(SelectionRadius);

	for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
	{
		if (It->TeamId != 0) continue;

		const FVector Center = It->GetFormationCenter();
		const float DistSq = (Center - ClickPos).SizeSquared2D();
		if (DistSq < BestDistSq)
		{
			BestDistSq  = DistSq;
			BestSpawner = *It;
		}
	}

	if (BestSpawner)
	{
		SelectedSpawner = BestSpawner;

		DrawDebugCircle(GetWorld(), BestSpawner->GetFormationCenter() + FVector(0.f, 0.f, 5.f),
			600.f, 48, FColor::Cyan, false, 0.5f, 0, 4.f,
			FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));
		return;
	}

	// ── Check if click is near an enemy formation → engage ──────────────
	if (SelectedSpawner)
	{
		ABattleSpawnerActor* BestEnemy = nullptr;
		float BestEnemyDistSq = FMath::Square(SelectionRadius);

		for (TActorIterator<ABattleSpawnerActor> It(GetWorld()); It; ++It)
		{
			if (It->TeamId == 0) continue;   // skip friendly

			const FVector EnemyCenter = It->GetFormationCenter();
			const float DistSq = (EnemyCenter - ClickPos).SizeSquared2D();
			if (DistSq < BestEnemyDistSq)
			{
				BestEnemyDistSq = DistSq;
				BestEnemy       = *It;
			}
		}

		if (BestEnemy)
		{
			// Red attack circle on the enemy
			DrawDebugCircle(GetWorld(),
				BestEnemy->GetFormationCenter() + FVector(0.f, 0.f, 5.f),
				500.f, 32, FColor::Red, false, 1.5f, 0, 4.f,
				FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));

			SelectedSpawner->IssueEngageOrder(BestEnemy);
			return;
		}
	}

	// Quick-move with current formation shape (clears engagement)
	if (SelectedSpawner)
	{
		DrawDebugCircle(GetWorld(), ClickPos + FVector(0.f, 0.f, 5.f),
			300.f, 32, FColor::Green, false, 1.5f, 0, 4.f,
			FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f));

		SelectedSpawner->IssueMoveOrder(ClickPos);
	}
}

// F1-F5 commands removed — now handled by clickable UI buttons in SBattleHUDWidget
