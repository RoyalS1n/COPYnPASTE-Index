#include "RWGameMode.h"

#include "RWCharacter.h"

ARWGameMode::ARWGameMode()
{
	DefaultPawnClass = ARWCharacter::StaticClass();
}
