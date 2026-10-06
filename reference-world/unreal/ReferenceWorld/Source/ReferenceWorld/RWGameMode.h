#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RWGameMode.generated.h"

/** Spawns the first-person explorer at the level's PlayerStart, with the RW HUD. */
UCLASS()
class REFERENCEWORLD_API ARWGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARWGameMode();
};
