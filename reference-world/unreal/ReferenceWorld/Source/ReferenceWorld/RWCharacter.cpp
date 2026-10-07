#include "RWCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

ARWCharacter::ARWCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	GetCapsuleComponent()->InitCapsuleSize(34.f, 90.f);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	Camera->bUsePawnControlRotation = true;
	Camera->SetFieldOfView(85.f);

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = WalkSpeed;
	Move->JumpZVelocity = 520.f;
	Move->AirControl = 0.35f;
	Move->MaxStepHeight = 50.f;            // castle stairs rise 25 cm per step
	Move->SetWalkableFloorAngle(50.f);     // the road and grassy slopes stay walkable
	Move->bOrientRotationToMovement = false;
}

void ARWCharacter::BeginPlay()
{
	Super::BeginPlay();
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void ARWCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	check(PlayerInputComponent);

	PlayerInputComponent->BindAxis(TEXT("MoveForward"), this, &ARWCharacter::MoveForward);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &ARWCharacter::MoveRight);
	PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ARWCharacter::Turn);
	PlayerInputComponent->BindAxis(TEXT("LookUp"), this, &ARWCharacter::LookUp);
	PlayerInputComponent->BindAxis(TEXT("TurnRate"), this, &ARWCharacter::TurnRate);
	PlayerInputComponent->BindAxis(TEXT("LookUpRate"), this, &ARWCharacter::LookUpRate);

	PlayerInputComponent->BindAction(TEXT("Jump"), IE_Pressed, this, &ACharacter::Jump);
	PlayerInputComponent->BindAction(TEXT("Jump"), IE_Released, this, &ACharacter::StopJumping);
	PlayerInputComponent->BindAction(TEXT("Sprint"), IE_Pressed, this, &ARWCharacter::StartSprint);
	PlayerInputComponent->BindAction(TEXT("Sprint"), IE_Released, this, &ARWCharacter::StopSprint);
}

void ARWCharacter::MoveForward(float Value)
{
	if (Controller && Value != 0.f)
	{
		const FRotator YawOnly(0.f, Controller->GetControlRotation().Yaw, 0.f);
		AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X), Value);
	}
}

void ARWCharacter::MoveRight(float Value)
{
	if (Controller && Value != 0.f)
	{
		const FRotator YawOnly(0.f, Controller->GetControlRotation().Yaw, 0.f);
		AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y), Value);
	}
}

void ARWCharacter::Turn(float Value)
{
	AddControllerYawInput(Value);
}

void ARWCharacter::LookUp(float Value)
{
	AddControllerPitchInput(Value);
}

void ARWCharacter::TurnRate(float Value)
{
	if (const UWorld* World = GetWorld())
	{
		AddControllerYawInput(Value * GamepadLookRate * World->GetDeltaSeconds());
	}
}

void ARWCharacter::LookUpRate(float Value)
{
	if (const UWorld* World = GetWorld())
	{
		AddControllerPitchInput(Value * GamepadLookRate * World->GetDeltaSeconds());
	}
}

void ARWCharacter::StartSprint()
{
	GetCharacterMovement()->MaxWalkSpeed = SprintSpeed;
}

void ARWCharacter::StopSprint()
{
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}
