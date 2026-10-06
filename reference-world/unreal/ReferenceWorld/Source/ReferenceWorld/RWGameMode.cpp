#include "RWGameMode.h"

#include "RWCharacter.h"
#include "RWHUD.h"

ARWGameMode::ARWGameMode()
{
	DefaultPawnClass = ARWCharacter::StaticClass();
	HUDClass = ARWHUD::StaticClass();
}
