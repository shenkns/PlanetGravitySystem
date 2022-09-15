// Copyright shenkns Planet Gravity Developed With Unreal Engine. All Rights Reserved 2022.

#include "GravityCharacter.h"
#include "GravityMovementComponent.h"
#include "GravityMath.h"

// Log categories
DEFINE_LOG_CATEGORY_STATIC(LogCharacter, Log, All);

AGravityCharacter::AGravityCharacter(const FObjectInitializer& ObjectInitializer) :
Super(ObjectInitializer.SetDefaultSubobjectClass<UGravityMovementComponent>(CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = false;

	GravityMovementComponent = Cast<UGravityMovementComponent>(GetCharacterMovement());
}

void AGravityCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Store current vertical axis and rotation
	LastAxisZ = GetActorAxisZ();
	LastRotation = GetActorQuat();

	// Subscribe to transform change event
	RootComponent->TransformUpdated.AddUObject(this, &ThisClass::TransformUpdated);
}

void AGravityCharacter::OnRep_ReplicatedBasedMovement()
{
	if (GetLocalRole() != ROLE_SimulatedProxy)
	{
		return;
	}

	// Skip base updates while playing root motion, it is handled inside of OnRep_RootMotion
	if (IsPlayingNetworkedRootMotionMontage())
	{
		return;
	}

	GetGravityMovementComponent()->bNetworkUpdateReceived = true;
	TGuardValue<bool> bInBaseReplicationGuard(bInBaseReplication, true);

	const bool bBaseChanged = (BasedMovement.MovementBase != ReplicatedBasedMovement.MovementBase || BasedMovement.BoneName != ReplicatedBasedMovement.BoneName);
	if (bBaseChanged)
	{
		// Even though we will copy the replicated based movement info, we need to use SetBase() to set up tick dependencies and trigger notifications
		SetBase(ReplicatedBasedMovement.MovementBase, ReplicatedBasedMovement.BoneName);
	}

	// Make sure to use the values of relative location/rotation etc from the server
	BasedMovement = ReplicatedBasedMovement;

	if (ReplicatedBasedMovement.HasRelativeLocation())
	{
		// Update transform relative to movement base
		const FVector OldLocation = GetActorLocation();
		const FQuat OldRotation = GetActorQuat();
		MovementBaseUtility::GetMovementBaseTransform(ReplicatedBasedMovement.MovementBase, ReplicatedBasedMovement.BoneName, GetGravityMovementComponent()->OldBaseLocation, GetGravityMovementComponent()->OldBaseQuat);
		const FVector NewLocation = GetGravityMovementComponent()->OldBaseLocation + ReplicatedBasedMovement.Location;
		FRotator NewRotation;

		if (ReplicatedBasedMovement.HasRelativeRotation())
		{
			// Relative location, relative rotation
			NewRotation = (FRotationMatrix(ReplicatedBasedMovement.Rotation) * FQuatRotationMatrix(GetGravityMovementComponent()->OldBaseQuat)).Rotator();

			if (GetGravityMovementComponent()->ShouldRemainVertical())
			{
				NewRotation = GetGravityMovementComponent()->ConstrainComponentRotation(NewRotation);
			}
		}
		else
		{
			// Relative location, absolute rotation
			NewRotation = ReplicatedBasedMovement.Rotation;
		}

		// When position or base changes, movement mode will need to be updated. This assumes rotation changes don't affect that
		GetGravityMovementComponent()->bJustTeleported |= (bBaseChanged || NewLocation != OldLocation);
		GetGravityMovementComponent()->bNetworkSmoothingComplete = false;
		GetGravityMovementComponent()->SmoothCorrection(OldLocation, OldRotation, NewLocation, NewRotation.Quaternion());
		OnUpdateSimulatedPosition(OldLocation, OldRotation);
	}
}

void AGravityCharacter::OnEndCrouch(float HeightAdjust, float ScaledHeightAdjust)
{
	RecalculateBaseEyeHeight();

	const AGravityCharacter* DefaultChar = GetDefault<AGravityCharacter>(GetClass());
	if (GetMesh() != nullptr && DefaultChar->GetMesh() != nullptr)
	{
		if (!GetMesh()->IsUsingAbsoluteLocation())
		{
			FVector& MeshRelativeLocation = GetMesh()->GetRelativeLocation_DirectMutable();
			MeshRelativeLocation.Z = DefaultChar->GetMesh()->GetRelativeLocation().Z;

			BaseTranslationOffset.Z = MeshRelativeLocation.Z;
		}
		else
		{
			BaseTranslationOffset.Z -= HeightAdjust;
		}
	}
	else
	{
		BaseTranslationOffset.Z = DefaultChar->BaseTranslationOffset.Z;
	}

	K2_OnEndCrouch(HeightAdjust, ScaledHeightAdjust);
}

void AGravityCharacter::OnStartCrouch(float HeightAdjust, float ScaledHeightAdjust)
{
	RecalculateBaseEyeHeight();

	const AGravityCharacter* DefaultChar = GetDefault<AGravityCharacter>(GetClass());
	if (GetMesh() != nullptr && DefaultChar->GetMesh() != nullptr)
	{
		if (!GetMesh()->IsUsingAbsoluteLocation())
		{
			FVector& MeshRelativeLocation = GetMesh()->GetRelativeLocation_DirectMutable();
			MeshRelativeLocation.Z = DefaultChar->GetMesh()->GetRelativeLocation().Z + HeightAdjust;

			BaseTranslationOffset.Z = MeshRelativeLocation.Z;
		}
		else
		{
			BaseTranslationOffset.Z += HeightAdjust;
		}
	}
	else
	{
		BaseTranslationOffset.Z = DefaultChar->BaseTranslationOffset.Z + HeightAdjust;
	}

	K2_OnStartCrouch(HeightAdjust, ScaledHeightAdjust);
}

void AGravityCharacter::ApplyDamageMomentum(float DamageTaken, const FDamageEvent& DamageEvent, APawn* PawnInstigator, AActor* DamageCauser)
{
	const UDamageType* DmgTypeCDO = DamageEvent.DamageTypeClass->GetDefaultObject<UDamageType>();
	const float ImpulseScale = DmgTypeCDO->DamageImpulse;

	UCharacterMovementComponent* CharMovement = GetCharacterMovement();
	if (ImpulseScale > 3.0f && CharMovement != nullptr)
	{
		FHitResult HitInfo;
		FVector ImpulseDir;
		DamageEvent.GetBestHitInfo(this, PawnInstigator, HitInfo, ImpulseDir);

		FVector Impulse = ImpulseDir * ImpulseScale;
		const bool bMassIndependentImpulse = !DmgTypeCDO->bScaleMomentumByMass;

		// Limit Z momentum added if already going up faster than jump (to avoid blowing character way up into the sky)
		{
			FVector MassScaledImpulse = Impulse;
			if (!bMassIndependentImpulse && CharMovement->Mass > SMALL_NUMBER)
			{
				MassScaledImpulse = MassScaledImpulse / CharMovement->Mass;
			}

			const FVector AxisZ = GetActorAxisZ();
			if ((CharMovement->Velocity | AxisZ) > GetDefault<UCharacterMovementComponent>(CharMovement->GetClass())->JumpZVelocity && (MassScaledImpulse | AxisZ) > 0.0f)
			{
				Impulse = FVector::VectorPlaneProject(Impulse, AxisZ) + AxisZ * ((Impulse | AxisZ) * 0.5f);
			}
		}

		CharMovement->AddImpulse(Impulse, bMassIndependentImpulse);
	}
}

FVector AGravityCharacter::GetPawnViewLocation() const
{
	return GetActorLocation() + GetActorAxisZ() * BaseEyeHeight;
}

void AGravityCharacter::FaceRotation(FRotator NewControlRotation, float DeltaTime)
{
	// If going to use yaw component of control rotation alone
	if (bUseControllerRotationYaw && !bUseControllerRotationPitch && !bUseControllerRotationRoll)
	{
		const FVector AxisZ = GetActorAxisZ();
		if (AxisZ.Z == 1.0f)
		{
			// Optimization; just use yaw rotation from the new control rotation
			const FRotator CurrentRotation = GetActorRotation();

			NewControlRotation.Pitch = CurrentRotation.Pitch;
			NewControlRotation.Roll = CurrentRotation.Roll;
		}
		else
		{
			const float CosineThreshold = (GetGravityMovementComponent() != nullptr) ?
				GetGravityMovementComponent()->GetThresholdParallelCosine() : NINJA_NORMALS_PARALLEL;

			NewControlRotation = FGravityMath::MakeFromZQuat(AxisZ, NewControlRotation.Quaternion(),
				CosineThreshold).Rotator();
		}

#if ENABLE_NAN_DIAGNOSTIC
		if (NewControlRotation.ContainsNaN())
		{
			logOrEnsureNanError(TEXT("AGravityCharacter::FaceRotation about to apply NaN-containing rotation to actor! New:(%s), Current:(%s)"),
				*NewControlRotation.ToString(), *GetActorRotation().ToString());
		}
#endif

		SetActorRotation(NewControlRotation);
	}
	else
	{
		Super::FaceRotation(NewControlRotation, DeltaTime);
	}
}

void AGravityCharacter::LaunchCharacterRotated(FVector LaunchVelocity, bool bHorizontalOverride, bool bVerticalOverride)
{
	UE_LOG(LogCharacter, Verbose, TEXT("AGravityCharacter::LaunchCharacterRotated '%s' %s"), *GetName(), *LaunchVelocity.ToCompactString());

	UCharacterMovementComponent* CharMovement = GetCharacterMovement();
	if (CharMovement != nullptr)
	{
		if (!bHorizontalOverride && !bVerticalOverride)
		{
			CharMovement->Launch(GetVelocity() + LaunchVelocity);
		}
		else if (bHorizontalOverride && bVerticalOverride)
		{
			CharMovement->Launch(LaunchVelocity);
		}
		else
		{
			FVector FinalVel;
			const FVector Velocity = GetVelocity();
			const FVector AxisZ = GetActorAxisZ();

			if (bHorizontalOverride)
			{
				FinalVel = FVector::VectorPlaneProject(LaunchVelocity, AxisZ) + AxisZ * (Velocity | AxisZ);
			}
			else // if (bVerticalOverride)
			{
				FinalVel = FVector::VectorPlaneProject(Velocity, AxisZ) + AxisZ * (LaunchVelocity | AxisZ);
			}

			CharMovement->Launch(FinalVel);
		}

		OnLaunched(LaunchVelocity, bHorizontalOverride, bVerticalOverride);
	}
}

void AGravityCharacter::TransformUpdated(USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	// Abort if rotation didn't change
	const FQuat NewRotation = GetActorQuat();
	if (NewRotation == LastRotation)
	{
		return;
	}

	const FVector NewAxisZ = GetActorAxisZ();
	const float CosineThreshold = (GetGravityMovementComponent() != nullptr) ?
		GetGravityMovementComponent()->GetThresholdParallelCosine() : NINJA_NORMALS_PARALLEL;

	// Abort if angle between new and old component 'up' axes almost equals to 0 degrees
	if (FGravityMath::Coincident(LastAxisZ, NewAxisZ, CosineThreshold))
	{
		return;
	}

	CharMovementAxisChanged(LastAxisZ, NewAxisZ);

	// Store current vertical axis and rotation
	LastAxisZ = NewAxisZ;
	LastRotation = NewRotation;
}

bool AGravityCharacter::SetCharMovementAxis(const FVector& NewAxisZ, bool bForceFindFloor)
{
	if (GetGravityMovementComponent() == nullptr)
	{
		return false;
	}

	// Try to set the new vertical axis
	return GetGravityMovementComponent()->SetComponentAxisZ(NewAxisZ, bForceFindFloor);
}

void AGravityCharacter::CharMovementAxisChanged(const FVector& OldAxisZ, const FVector& CurrentAxisZ)
{
	OnCharMovementAxisChanged(OldAxisZ, CurrentAxisZ);

	CharMovementAxisChangedDelegate.Broadcast(OldAxisZ, CurrentAxisZ);
	K2_OnCharMovementAxisChanged(OldAxisZ, CurrentAxisZ);
}

void AGravityCharacter::OnCharMovementAxisChanged(const FVector& OldAxisZ, const FVector& CurrentAxisZ)
{
	if (bCapsuleRotatesControlRotation && Controller != nullptr)
	{
		const FQuat ControlRotation = Controller->GetControlRotation().Quaternion();
		FQuat QuatRotation;

		const float CosineThreshold = (GetGravityMovementComponent() != nullptr) ?
			GetGravityMovementComponent()->GetThresholdParallelCosine() : NINJA_NORMALS_PARALLEL;

		// Figure out if angle between new and old 'up' axes is less than 180 degrees
		if (!FGravityMath::Opposite(CurrentAxisZ, OldAxisZ, CosineThreshold))
		{
			// Obtain quaternion rotation difference between 'up' axes
			QuatRotation = FQuat::FindBetweenNormals(OldAxisZ, CurrentAxisZ);
		}
		else
		{
			// Flip control rotation by preserving forward axis
			QuatRotation = FQuat(FGravityMath::GetAxisX(ControlRotation), PI);
		}

		Controller->SetControlRotation((QuatRotation * ControlRotation).Rotator());
	}
}

void AGravityCharacter::GravityDirectionChanged(ENinjaGravityDirectionMode OldGravityDirectionMode, ENinjaGravityDirectionMode CurrentGravityDirectionMode)
{
	OnGravityDirectionChanged(OldGravityDirectionMode, CurrentGravityDirectionMode);

	GravityDirectionChangedDelegate.Broadcast(OldGravityDirectionMode, CurrentGravityDirectionMode);
	K2_OnGravityDirectionChanged(OldGravityDirectionMode, CurrentGravityDirectionMode);
}

void AGravityCharacter::OnGravityDirectionChanged(ENinjaGravityDirectionMode OldGravityDirectionMode, ENinjaGravityDirectionMode CurrentGravityDirectionMode)
{
}

void AGravityCharacter::UnwalkableHit(const FHitResult& Hit)
{
	OnUnwalkableHit(Hit);

	UnwalkableHitDelegate.Broadcast(Hit);
	K2_OnUnwalkableHit(Hit);
}

void AGravityCharacter::OnUnwalkableHit(const FHitResult& Hit)
{
}

FVector AGravityCharacter::GetActorAxisX() const
{
	if (GetGravityMovementComponent() != nullptr && GetGravityMovementComponent()->UpdatedComponent == RootComponent)
	{
		return GetGravityMovementComponent()->GetComponentAxisX();
	}

	return GetActorQuat().GetAxisX();
}

FVector AGravityCharacter::GetActorAxisY() const
{
	if (GetGravityMovementComponent() != nullptr && GetGravityMovementComponent()->UpdatedComponent == RootComponent)
	{
		return GetGravityMovementComponent()->GetComponentAxisY();
	}

	return GetActorQuat().GetAxisY();
}

FVector AGravityCharacter::GetActorAxisZ() const
{
	if (GetGravityMovementComponent() != nullptr && GetGravityMovementComponent()->UpdatedComponent == RootComponent)
	{
		return GetGravityMovementComponent()->GetComponentAxisZ();
	}

	return GetActorQuat().GetAxisZ();
}

void AGravityCharacter::SmoothComponentLocationAndRotation(class USceneComponent* SceneComponent, float DeltaTime, float LocationSpeed, float RotationSpeed, const FVector& RelativeLocation, const FRotator& RelativeRotation)
{
	if (DeltaTime <= 0.0f || SceneComponent == nullptr)
	{
		return;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// Warn and don't smooth Mesh if NetworkSmoothingMode of CharacterMovement is enabled
	if (SceneComponent == GetMesh() && GetLocalRole() == ROLE_SimulatedProxy)
	{
		const UCharacterMovementComponent* CharMovement = GetCharacterMovement();
		if (CharMovement != nullptr && CharMovement->NetworkSmoothingMode != ENetworkSmoothingMode::Disabled)
		{
			ensureMsgf(false,
				TEXT("SmoothComponentLocationAndRotation: disable NetworkSmoothingMode in CharacterMovementComponent of %s"),
				*GetClass()->GetName());
			return;
		}
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	SceneComponent->SetUsingAbsoluteLocation(true);
	SceneComponent->SetUsingAbsoluteRotation(true);

	FQuat NewRotation = GetActorQuat() * (RelativeRotation.IsNearlyZero() ?
		FQuat::Identity : RelativeRotation.Quaternion());

	if (RotationSpeed > 0.0f)
	{
		NewRotation = FMath::QInterpTo(SceneComponent->GetComponentQuat(),
			NewRotation, DeltaTime, RotationSpeed);
	}

	FVector NewLocation = GetActorLocation() + (RelativeLocation.IsNearlyZero() ?
		FVector::ZeroVector : NewRotation.RotateVector(RelativeLocation));

	if (LocationSpeed > 0.0f)
	{
		NewLocation = FMath::VInterpTo(SceneComponent->GetComponentLocation(),
			NewLocation, DeltaTime, LocationSpeed);
	}

	SceneComponent->SetWorldLocationAndRotation(NewLocation, NewRotation);
}

void AGravityCharacter::SmoothComponentLocation(class USceneComponent* SceneComponent, float DeltaTime, float LocationSpeed, const FVector& RelativeLocation)
{
	if (DeltaTime <= 0.0f || SceneComponent == nullptr)
	{
		return;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// Warn and don't smooth Mesh if NetworkSmoothingMode of CharacterMovement is enabled
	if (SceneComponent == GetMesh() && GetLocalRole() == ROLE_SimulatedProxy)
	{
		const UCharacterMovementComponent* CharMovement = GetCharacterMovement();
		if (CharMovement != nullptr && CharMovement->NetworkSmoothingMode != ENetworkSmoothingMode::Disabled)
		{
			ensureMsgf(false,
				TEXT("SmoothComponentLocation: disable NetworkSmoothingMode in CharacterMovementComponent of %s"),
				*GetClass()->GetName());
			return;
		}
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	SceneComponent->SetUsingAbsoluteLocation(true);

	FVector NewLocation = GetActorLocation() + (RelativeLocation.IsNearlyZero() ?
		FVector::ZeroVector : GetActorQuat().RotateVector(RelativeLocation));

	if (LocationSpeed > 0.0f)
	{
		NewLocation = FMath::VInterpTo(SceneComponent->GetComponentLocation(),
			NewLocation, DeltaTime, LocationSpeed);
	}

	SceneComponent->SetWorldLocation(NewLocation);
}

void AGravityCharacter::SmoothComponentRotation(class USceneComponent* SceneComponent, float DeltaTime, float RotationSpeed, const FRotator& RelativeRotation)
{
	if (DeltaTime <= 0.0f || SceneComponent == nullptr)
	{
		return;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// Warn and don't smooth Mesh if NetworkSmoothingMode of CharacterMovement is enabled
	if (SceneComponent == GetMesh() && GetLocalRole() == ROLE_SimulatedProxy)
	{
		const UCharacterMovementComponent* CharMovement = GetCharacterMovement();
		if (CharMovement != nullptr && CharMovement->NetworkSmoothingMode != ENetworkSmoothingMode::Disabled)
		{
			ensureMsgf(false,
				TEXT("SmoothComponentRotation: disable NetworkSmoothingMode in CharacterMovementComponent of %s"),
				*GetClass()->GetName());
			return;
		}
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	SceneComponent->SetUsingAbsoluteRotation(true);

	FQuat NewRotation = GetActorQuat() * (RelativeRotation.IsNearlyZero() ?
		FQuat::Identity : RelativeRotation.Quaternion());

	if (RotationSpeed > 0.0f)
	{
		NewRotation = FMath::QInterpTo(SceneComponent->GetComponentQuat(),
			NewRotation, DeltaTime, RotationSpeed);
	}

	SceneComponent->SetWorldRotation(NewRotation);
}
