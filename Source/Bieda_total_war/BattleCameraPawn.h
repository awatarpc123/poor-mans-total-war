#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "InputActionValue.h"
#include "BattleCameraPawn.generated.h"

class UInputAction;
class UInputMappingContext;
class ABattleSpawnerActor;

/**
 * Free camera for observing and commanding the battle.
 *
 * Controls:
 *   WASD             — pan camera (horizontal)
 *   E / Q            — move up / down
 *   Shift            — speed boost
 *   RMB (hold)       — rotate camera (hides cursor)
 *   LMB (click)      — select unit / quick move order (keep formation)
 *   LMB (drag)       — drag-to-form: draw front line, set formation width + facing
 *   ESC              — deselect current unit
 */
UCLASS()
class BIEDA_TOTAL_WAR_API ABattleCameraPawn : public ASpectatorPawn
{
	GENERATED_BODY()

public:
	ABattleCameraPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Public getter/setter for HUD & UI. */
	ABattleSpawnerActor* GetSelectedSpawner() const { return SelectedSpawner; }
	void SetSelectedSpawner(ABattleSpawnerActor* S) { SelectedSpawner = S; }

private:
	// ── Input actions ────────────────────────────────────────────────────────
	UPROPERTY() TObjectPtr<UInputAction> MoveForwardAction;
	UPROPERTY() TObjectPtr<UInputAction> MoveRightAction;
	UPROPERTY() TObjectPtr<UInputAction> MoveUpAction;
	UPROPERTY() TObjectPtr<UInputAction> LookAction;
	UPROPERTY() TObjectPtr<UInputAction> SpeedBoostAction;
	UPROPERTY() TObjectPtr<UInputAction> LMBAction;
	UPROPERTY() TObjectPtr<UInputAction> RMBAction;
	UPROPERTY() TObjectPtr<UInputAction> ESCAction;

	UPROPERTY() TObjectPtr<UInputMappingContext> BattleIMC;

	// ── State ────────────────────────────────────────────────────────────────
	float NormalSpeed = 2000.f;
	float TurboSpeed  = 6000.f;
	bool  bRMBHeld    = false;

	// ── Unit selection ───────────────────────────────────────────────────────
	UPROPERTY()
	TObjectPtr<ABattleSpawnerActor> SelectedSpawner;

	// Max distance from the click to a squad's NEAREST soldier to select it.
	// Measured to the closest man (not the formation centre), so it stays small —
	// a click just has to land near someone in the unit, on any flank.
	float SelectionRadius = 400.f;

	// ── Drag-to-form state ───────────────────────────────────────────────────
	bool    bLMBHeld      = false;
	bool    bIsDragging   = false;
	FVector DragStartPos  = FVector::ZeroVector;   // ground position at LMB down
	float   DragThreshold = 150.f;                 // cm on ground before drag activates

	// ── Helpers ──────────────────────────────────────────────────────────────
	void BuildInputSetup();
	void ApplyMappingContext();
	bool GetGroundHitUnderCursor(FVector& OutHit) const;
	bool IsCursorOverUI() const;
	void DrawFormationPreview(const FVector& Center, int32 RowSize, int32 NumAgents,
		float Spacing, const FVector& FrontLineDir);

	// ── Handlers ─────────────────────────────────────────────────────────────
	void OnMoveForward  (const FInputActionValue& Value);
	void OnMoveRight    (const FInputActionValue& Value);
	void OnMoveUp       (const FInputActionValue& Value);
	void OnLook         (const FInputActionValue& Value);
	void OnSpeedBoostStart(const FInputActionValue& Value);
	void OnSpeedBoostEnd  (const FInputActionValue& Value);
	void OnLMBDown      (const FInputActionValue& Value);
	void OnLMBUp        (const FInputActionValue& Value);
	void OnRMBStart     (const FInputActionValue& Value);
	void OnRMBEnd       (const FInputActionValue& Value);
	void OnESCPress     (const FInputActionValue& Value);
};
