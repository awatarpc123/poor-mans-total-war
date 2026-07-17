#include "BattleSimControl.h"
#include "HAL/IConsoleManager.h"

// Console-driven so it can also be toggled from the `~` console (bieda.Pause 1)
// and inspected/automated. The keyboard toggle just flips this CVar.
static TAutoConsoleVariable<int32> CVarBiedaPause(
	TEXT("bieda.Pause"),
	0,
	TEXT("Tactical pause: freeze the battle simulation while camera/orders stay live (0=run, 1=pause)."),
	ECVF_Default);

bool BattleSimPaused()
{
	return CVarBiedaPause.GetValueOnAnyThread() != 0;
}

void SetBattleSimPaused(bool bPaused)
{
	CVarBiedaPause->Set(bPaused ? 1 : 0, ECVF_SetByCode);
}

void ToggleBattleSimPaused()
{
	SetBattleSimPaused(!BattleSimPaused());
}

// ── Game speed ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<float> CVarBiedaTimeScale(
	TEXT("bieda.TimeScale"),
	1.0f,
	TEXT("Battle simulation speed multiplier (slow-mo / fast-forward). Camera unaffected."),
	ECVF_Default);

float BattleSimTimeScale()
{
	return FMath::Max(0.f, CVarBiedaTimeScale.GetValueOnAnyThread());
}

void StepBattleSimTimeScale(int32 Dir)
{
	// Fixed, predictable steps so the UI can show a clean "x2" etc.
	static const float Steps[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
	const int32 N = UE_ARRAY_COUNT(Steps);

	// Find the nearest current step, then move by Dir within bounds.
	const float Cur = BattleSimTimeScale();
	int32 Idx = 2;   // default to 1.0
	float Best = FLT_MAX;
	for (int32 i = 0; i < N; ++i)
	{
		const float D = FMath::Abs(Steps[i] - Cur);
		if (D < Best) { Best = D; Idx = i; }
	}
	Idx = FMath::Clamp(Idx + (Dir > 0 ? 1 : -1), 0, N - 1);
	CVarBiedaTimeScale->Set(Steps[Idx], ECVF_SetByCode);
}

// ── Debug capsules ────────────────────────────────────────────────────────────
static TAutoConsoleVariable<int32> CVarBiedaDebugCapsules(
	TEXT("bieda.DebugCapsules"),
	0,
	TEXT("When 1, all squads render as engine cylinders instead of skeletal meshes.\n")
	TEXT("Use this to isolate rendering cost from simulation cost. Set to 1 and\n")
	TEXT("re-run SetupVisualization (next Tick)."),
	ECVF_Default);

bool BattleDebugCapsulesEnabled()
{
	return CVarBiedaDebugCapsules.GetValueOnAnyThread() != 0;
}
