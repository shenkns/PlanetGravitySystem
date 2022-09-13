// Copyright shenkns Planet Gravity Developed With Unreal Engine. All Rights Reserved 2022.

#pragma once

#include "GameFramework/Character.h"
#include "GravityCharacter.generated.h"

class UGravityMovementComponent;

UCLASS()
class PLANETGRAVITY_API AGravityCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	
	AGravityCharacter(const FObjectInitializer& ObjectInitializer);

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UGravityMovementComponent* GravityMovementComponent;

public:

	UFUNCTION(BlueprintPure, Category = "Gravity")
	UGravityMovementComponent* GetGravityMovementComponent() const { return GravityMovementComponent; };
};
