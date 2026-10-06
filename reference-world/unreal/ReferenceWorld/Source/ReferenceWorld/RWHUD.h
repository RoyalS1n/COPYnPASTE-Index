#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "RWHUD.generated.h"

/** Minimal HUD: a centre dot and a controls hint that fades out. */
UCLASS()
class REFERENCEWORLD_API ARWHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

protected:
	/** Seconds the controls hint stays on screen after spawning. */
	UPROPERTY(EditAnywhere, Category = "HUD")
	float HintSeconds = 12.f;
};
