#pragma once

#include "CoreMinimal.h"

/**
 * Drives the level-editor camera so entering the world feels continuous:
 *
 *   preview  -> camera high above the map, whole scene visible
 *   start    -> 900 ms eased push from that overview down to the ego's
 *               overhead chase position, while the panel collapses
 *   running  -> smoothed follow of the pose the backend reports
 *   stop     -> 1000 ms eased pull back out to the map overview
 *
 * Positions come from the backend in CARLA/UE world units (centimetres), so the
 * rig writes them straight into the active editor viewport.
 */
class FPreviewCameraRig
{
public:
	enum class EMode : uint8
	{
		None,
		Preview,
		ToFollow,
		Follow,
		ToPreview
	};

	/** Overview pose for the loaded map (from /sessions/current cameras.preview). */
	void SetPreviewPose(const FVector& Location, const FRotator& Rotation);

	/** Live ego pose (from /sessions/current cameras.follow). */
	void SetFollowPose(const FVector& Location, const FRotator& Rotation);

	/** Ease from wherever the camera is now to the stored overview pose. */
	void EnterPreview(float Duration = 1.0f);

	/** Ease from wherever the camera is now to the current follow pose. */
	void EnterFollow(float Duration = 0.9f);

	/** Jump straight to the overview pose (first frame, or after a map change). */
	void SnapToPreview();

	void Tick(float DeltaSeconds);

	bool HasPreviewPose() const { return bHasPreview; }
	bool HasFollowPose() const { return bHasFollow; }
	EMode GetMode() const { return Mode; }
	FString DescribeMode() const;

	/** Current interpolated pose; the module feeds it to the runtime camera actor. */
	/** Mouse control of the overview: drag pans, wheel zooms. */
	void AddUserPan(const FVector2D& ScreenDelta);
	void AddUserZoom(float WheelDelta);
	void ResetUserView();
	bool HasUserViewOffset() const { return !UserPan.IsNearlyZero() || !FMath::IsNearlyEqual(UserZoom, 1.f); }

	const FVector& GetCurrentLocation() const { return CurrentLocation; }
	const FRotator& GetCurrentRotation() const { return CurrentRotation; }
	bool HasCurrentPose() const { return bHasCurrent; }

private:
	static float EaseInOutCubic(float Alpha);

	FVector PreviewLocation = FVector::ZeroVector;
	FRotator PreviewRotation = FRotator::ZeroRotator;
	bool bHasPreview = false;

	FVector FollowLocation = FVector::ZeroVector;
	FRotator FollowRotation = FRotator::ZeroRotator;
	bool bHasFollow = false;

	FVector CurrentLocation = FVector::ZeroVector;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	bool bHasCurrent = false;

	FVector TransitionFromLocation = FVector::ZeroVector;
	FRotator TransitionFromRotation = FRotator::ZeroRotator;

	FVector2D UserPan = FVector2D::ZeroVector;
	float UserZoom = 1.f;
	EMode Mode = EMode::None;
	float TransitionElapsed = 0.f;
	float TransitionDuration = 0.9f;
};

