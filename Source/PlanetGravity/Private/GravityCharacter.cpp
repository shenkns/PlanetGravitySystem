// Copyright shenkns Planet Gravity Developed With Unreal Engine. All Rights Reserved 2022.

#include "GravityCharacter.h"
#include "GravityMovementComponent.h"

AGravityCharacter::AGravityCharacter(const FObjectInitializer& ObjectInitializer) :
Super(ObjectInitializer.SetDefaultSubobjectClass<UGravityMovementComponent>(CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = false;

	GravityMovementComponent = Cast<UGravityMovementComponent>(GetCharacterMovement());
}
