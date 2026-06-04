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
