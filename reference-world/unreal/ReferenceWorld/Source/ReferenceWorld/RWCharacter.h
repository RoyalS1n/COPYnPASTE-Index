#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RWCharacter.generated.h"

class UCameraComponent;

/**
 * First-person explorer: walk, sprint, jump, mouse / gamepad look.
 * Uses the axis and action mappings in Config/DefaultInput.ini.
 */
UCLASS()
class REFERENCEWORLD_API ARWCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ARWCharacter();

protected:
	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	void MoveForward(float Value);
	void MoveRight(float Value);
	void Turn(float Value);
	void LookUp(float Value);
	void TurnRate(float Value);
	void LookUpRate(float Value);
	void StartSprint();
	void StopSprint();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float WalkSpeed = 450.f;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float SprintSpeed = 950.f;

	/** Degrees per second at full stick deflection. */
	UPROPERTY(EditAnywhere, Category = "Input")
	float GamepadLookRate = 120.f;
};
