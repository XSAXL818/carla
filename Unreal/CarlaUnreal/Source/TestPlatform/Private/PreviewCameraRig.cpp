#include "PreviewCameraRig.h"

namespace
{
	/** Follow smoothing: fraction of the remaining gap closed per 1/60 s. */
	constexpr float FollowSmoothing = 0.18f;
}

void FPreviewCameraRig::SetPreviewPose(const FVector& Location, const FRotator& Rotation)
{
	const bool bMoved = !bHasPreview ||
		!PreviewLocation.Equals(Location, 1.0f) ||
		!PreviewRotation.Equals(Rotation, 1.0f);
	PreviewLocation = Location;
	PreviewRotation = Rotation;
	bHasPreview = true;

	// A new map moved the overview: re-snap instead of drifting there.
	if (bMoved && (Mode == EMode::Preview || Mode == EMode::None))
	{
		SnapToPreview();
	}
}

void FPreviewCameraRig::SetFollowPose(const FVector& Location, const FRotator& Rotation)
{
	FollowLocation = Location;
	FollowRotation = Rotation;
	bHasFollow = true;
}

void FPreviewCameraRig::EnterPreview(float Duration)
{
	if (!bHasPreview)
	{
		return;
	}
	TransitionFromLocation = bHasCurrent ? CurrentLocation : PreviewLocation;
	TransitionFromRotation = bHasCurrent ? CurrentRotation : PreviewRotation;
	TransitionElapsed = 0.f;
	TransitionDuration = FMath::Max(0.05f, Duration);
	Mode = EMode::ToPreview;
}

void FPreviewCameraRig::EnterFollow(float Duration)
{
	if (!bHasFollow)
	{
		// no pose yet: keep the overview until the backend reports the ego
		Mode = EMode::ToFollow;
		TransitionElapsed = 0.f;
		TransitionDuration = FMath::Max(0.05f, Duration);
		TransitionFromLocation = bHasCurrent ? CurrentLocation : PreviewLocation;
		TransitionFromRotation = bHasCurrent ? CurrentRotation : PreviewRotation;
		return;
	}
	TransitionFromLocation = bHasCurrent ? CurrentLocation : PreviewLocation;
	TransitionFromRotation = bHasCurrent ? CurrentRotation : PreviewRotation;
	TransitionElapsed = 0.f;
	TransitionDuration = FMath::Max(0.05f, Duration);
	Mode = EMode::ToFollow;
}

void FPreviewCameraRig::SnapToPreview()
{
	if (!bHasPreview)
	{
		return;
	}
	CurrentLocation = PreviewLocation;
	CurrentRotation = PreviewRotation;
	bHasCurrent = true;
	Mode = EMode::Preview;
	TransitionElapsed = 0.f;
}

void FPreviewCameraRig::AddUserPan(const FVector2D& ScreenDelta)
{
	// "grab the map" semantics: the content follows the cursor, so dragging right
	// moves the camera left (and dragging down moves the camera up).
	// Scale with altitude so a drag feels the same whether we are high or low.
	const float Scale = FMath::Max(100.0f, PreviewLocation.Z) * 0.0016f;
	const FRotationMatrix Basis(PreviewRotation);
	const FVector ScreenRight = Basis.GetUnitAxis(EAxis::Y);
	const FVector ScreenUp = Basis.GetUnitAxis(EAxis::Z);
	const FVector Delta =
		-ScreenRight * (ScreenDelta.X * Scale) + ScreenUp * (-ScreenDelta.Y * Scale);
	UserPan += FVector2D(Delta.X, Delta.Y);
	UserPan.X = FMath::Clamp(UserPan.X, -60000.0f, 60000.0f);
	UserPan.Y = FMath::Clamp(UserPan.Y, -60000.0f, 60000.0f);
	if (Mode == EMode::Preview)
	{
		CurrentLocation = FVector(PreviewLocation.X + UserPan.X,
			PreviewLocation.Y + UserPan.Y, PreviewLocation.Z * UserZoom);
	}
}

void FPreviewCameraRig::AddUserZoom(float WheelDelta)
{
	UserZoom = FMath::Clamp(UserZoom * (1.f - WheelDelta * 0.12f), 0.12f, 3.0f);
	if (Mode == EMode::Preview)
	{
		CurrentLocation = FVector(PreviewLocation.X + UserPan.X,
			PreviewLocation.Y + UserPan.Y, PreviewLocation.Z * UserZoom);
	}
}

void FPreviewCameraRig::ResetUserView()
{
	UserPan = FVector2D::ZeroVector;
	UserZoom = 1.f;
	if (Mode == EMode::Preview)
	{
		CurrentLocation = PreviewLocation;
	}
}

void FPreviewCameraRig::Tick(float DeltaSeconds)
{
	if (!bHasCurrent && bHasPreview)
	{
		SnapToPreview();
	}
	if (!bHasCurrent)
	{
		return;
	}

	switch (Mode)
	{
	case EMode::ToPreview:
	{
		if (!bHasPreview)
		{
			Mode = EMode::None;
			break;
		}
		TransitionElapsed += DeltaSeconds;
		const float Alpha = FMath::Clamp(TransitionElapsed / TransitionDuration, 0.f, 1.f);
		const float Eased = EaseInOutCubic(Alpha);
		CurrentLocation = FMath::Lerp(TransitionFromLocation, PreviewLocation, Eased);
		CurrentRotation = FMath::Lerp(TransitionFromRotation, PreviewRotation, Eased);
		if (Alpha >= 1.f)
		{
			Mode = EMode::Preview;
		}
		break;
	}
	case EMode::ToFollow:
	{
		if (!bHasFollow)
		{
			break; // wait for the first ego pose
		}
		TransitionElapsed += DeltaSeconds;
		const float Alpha = FMath::Clamp(TransitionElapsed / TransitionDuration, 0.f, 1.f);
		const float Eased = EaseInOutCubic(Alpha);
		// the ego moves during the transition, so ease toward its *current* pose
		CurrentLocation = FMath::Lerp(TransitionFromLocation, FollowLocation, Eased);
		CurrentRotation = FMath::Lerp(TransitionFromRotation, FollowRotation, Eased);
		if (Alpha >= 1.f)
		{
			Mode = EMode::Follow;
		}
		break;
	}
	case EMode::Follow:
	{
		if (!bHasFollow)
		{
			break;
		}
		const float Blend = 1.f - FMath::Pow(1.f - FollowSmoothing, DeltaSeconds * 60.f);
		CurrentLocation = FMath::Lerp(CurrentLocation, FollowLocation, Blend);
		CurrentRotation = FMath::Lerp(CurrentRotation, FollowRotation, Blend);
		break;
	}
	case EMode::Preview:
		if (bHasPreview)
		{
			CurrentLocation = FVector(PreviewLocation.X + UserPan.X,
				PreviewLocation.Y + UserPan.Y, PreviewLocation.Z * UserZoom);
			CurrentRotation = PreviewRotation;
			bHasCurrent = true;
		}
		break;
	case EMode::None:
	default:
		break;
	}
}

float FPreviewCameraRig::EaseInOutCubic(float Alpha)
{
	return Alpha < 0.5f
		? 4.f * Alpha * Alpha * Alpha
		: 1.f - FMath::Pow(-2.f * Alpha + 2.f, 3.f) / 2.f;
}

FString FPreviewCameraRig::DescribeMode() const
{
	switch (Mode)
	{
	case EMode::Preview: return TEXT("preview");
	case EMode::ToFollow: return TEXT("->follow");
	case EMode::Follow: return TEXT("follow");
	case EMode::ToPreview: return TEXT("->preview");
	default: return TEXT("none");
	}
}


