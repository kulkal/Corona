#include "stdafx.h"
#include "Corona.h"
#include "PlatformSystem.h"
#include "Utils.h"
#include "external/streamline-sdk/external/json/include/nlohmann/json.hpp"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

void AppendCpuRuntimeTrace(const std::wstring& line);
namespace
{
	constexpr int kVehicleWheelFL = 0;
	constexpr int kVehicleWheelFR = 1;
	constexpr int kVehicleWheelRL = 2;
	constexpr int kVehicleWheelRR = 3;
	constexpr uint16_t kVehicleGamepadButtonA = 0x1000u;
	constexpr uint16_t kVehicleGamepadButtonX = 0x4000u;

	glm::vec3 VehicleForwardFromYaw(float yaw)
	{
		return glm::normalize(glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)));
	}

	glm::vec3 VehicleRightFromYaw(float yaw)
	{
		return glm::normalize(glm::vec3(std::cos(yaw), 0.0f, -std::sin(yaw)));
	}

	float VehicleSignNonZero(float v)
	{
		return v >= 0.0f ? 1.0f : -1.0f;
	}

	constexpr float kVehicleVisualScale = 0.2f;

	glm::mat4x4 BuildVehiclePartTransform(
		const shared_ptr<Scene>& scene,
		const glm::vec3& scale,
		const glm::vec3& center,
		float yaw,
		float pitch,
		float roll)
	{
		if (!scene || !scene->bHasBounds)
			return glm::mat4x4(1.0f);

		const glm::vec3 boundsCenter = (scene->BoundsMin + scene->BoundsMax) * 0.5f;
		const glm::mat4x4 rotation =
			glm::rotate(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::rotate(pitch, glm::vec3(1.0f, 0.0f, 0.0f)) *
			glm::rotate(roll, glm::vec3(0.0f, 0.0f, 1.0f));

		return
			glm::translate(center) *
			rotation *
			glm::scale(scale) *
			glm::translate(-boundsCenter);
	}

	glm::mat4x4 BuildVehicleWheelTransform(
		const shared_ptr<Scene>& scene,
		const glm::vec3& scale,
		const glm::vec3& center,
		float steerYaw,
		float camber,
		float spin)
	{
		if (!scene || !scene->bHasBounds)
			return glm::mat4x4(1.0f);

		const glm::vec3 boundsCenter = (scene->BoundsMin + scene->BoundsMax) * 0.5f;
		const glm::mat4x4 rotation =
			glm::rotate(steerYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::rotate(camber, glm::vec3(0.0f, 0.0f, 1.0f)) *
			glm::rotate(spin, glm::vec3(1.0f, 0.0f, 0.0f));

		return
			glm::translate(center) *
			rotation *
			glm::scale(scale) *
			glm::translate(-boundsCenter);
	}

	glm::vec3 RotateVehicleLocal(float yaw, float pitch, float roll, const glm::vec3& local)
	{
		const glm::mat4x4 rotation =
			glm::rotate(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
			glm::rotate(pitch, glm::vec3(1.0f, 0.0f, 0.0f)) *
			glm::rotate(roll, glm::vec3(0.0f, 0.0f, 1.0f));
		return glm::vec3(rotation * glm::vec4(local, 0.0f));
	}
}

std::wstring Corona::GetVehicleAlignmentSettingsPath() const
{
	return RuntimePaths::ConfigFile(L"vehicle_alignment.json").wstring();
}

void Corona::ClampVehicleAlignmentSettings()
{
	VehicleAlignment.FrontStaticToeDeg = std::clamp(VehicleAlignment.FrontStaticToeDeg, -5.0f, 5.0f);
	VehicleAlignment.RearStaticToeDeg = std::clamp(VehicleAlignment.RearStaticToeDeg, -5.0f, 5.0f);
	VehicleAlignment.FrontStaticNegativeCamberDeg = std::clamp(VehicleAlignment.FrontStaticNegativeCamberDeg, 0.0f, 10.0f);
	VehicleAlignment.RearStaticNegativeCamberDeg = std::clamp(VehicleAlignment.RearStaticNegativeCamberDeg, 0.0f, 10.0f);
	VehicleAlignment.FrontRollDynamicCamberDeg = std::clamp(VehicleAlignment.FrontRollDynamicCamberDeg, 0.0f, 12.0f);
	VehicleAlignment.RearRollDynamicCamberDeg = std::clamp(VehicleAlignment.RearRollDynamicCamberDeg, 0.0f, 12.0f);
	VehicleAlignment.FrontRollToeGain = std::clamp(VehicleAlignment.FrontRollToeGain, -1.0f, 1.0f);
	VehicleAlignment.RearRollToeGain = std::clamp(VehicleAlignment.RearRollToeGain, -1.0f, 1.0f);
	VehicleAlignment.FrontBrakeToeStabilize = std::clamp(VehicleAlignment.FrontBrakeToeStabilize, 0.0f, 2.0f);
	VehicleAlignment.RearBrakeToeStabilize = std::clamp(VehicleAlignment.RearBrakeToeStabilize, 0.0f, 0.20f);
	VehicleAlignment.VisualAlignmentScale = std::clamp(VehicleAlignment.VisualAlignmentScale, 0.0f, 5.0f);
}

bool Corona::LoadVehicleAlignmentSettings()
{
	const std::filesystem::path path(GetVehicleAlignmentSettingsPath());
	std::ifstream file(path);
	if (!file.is_open())
	{
		ClampVehicleAlignmentSettings();
		const bool bSavedDefaults = SaveVehicleAlignmentSettings();
		VehicleAlignmentStatus =
			bSavedDefaults ?
			L"Created default alignment JSON: " + path.wstring() :
			L"Using default alignment; failed to create JSON: " + path.wstring();
		return bSavedDefaults;
	}

	try
	{
		nlohmann::json root;
		file >> root;
		VehicleAlignment.FrontStaticToeDeg = root.value("front_static_toe_deg", VehicleAlignment.FrontStaticToeDeg);
		VehicleAlignment.RearStaticToeDeg = root.value("rear_static_toe_deg", VehicleAlignment.RearStaticToeDeg);
		VehicleAlignment.FrontStaticNegativeCamberDeg = root.value("front_static_negative_camber_deg", VehicleAlignment.FrontStaticNegativeCamberDeg);
		VehicleAlignment.RearStaticNegativeCamberDeg = root.value("rear_static_negative_camber_deg", VehicleAlignment.RearStaticNegativeCamberDeg);
		VehicleAlignment.FrontRollDynamicCamberDeg = root.value("front_roll_dynamic_camber_deg", VehicleAlignment.FrontRollDynamicCamberDeg);
		VehicleAlignment.RearRollDynamicCamberDeg = root.value("rear_roll_dynamic_camber_deg", VehicleAlignment.RearRollDynamicCamberDeg);
		VehicleAlignment.FrontRollToeGain = root.value("front_roll_toe_gain", VehicleAlignment.FrontRollToeGain);
		VehicleAlignment.RearRollToeGain = root.value("rear_roll_toe_gain", VehicleAlignment.RearRollToeGain);
		VehicleAlignment.FrontBrakeToeStabilize = root.value("front_brake_toe_stabilize", VehicleAlignment.FrontBrakeToeStabilize);
		VehicleAlignment.RearBrakeToeStabilize = root.value("rear_brake_toe_stabilize", VehicleAlignment.RearBrakeToeStabilize);
		VehicleAlignment.VisualAlignmentScale = root.value("visual_alignment_scale", VehicleAlignment.VisualAlignmentScale);
		ClampVehicleAlignmentSettings();
		VehicleAlignmentStatus = L"Loaded alignment JSON: " + path.wstring();
		AppendCpuRuntimeTrace(L"[VehicleAlignment] loaded path=" + path.wstring());
		return true;
	}
	catch (const std::exception& e)
	{
		ClampVehicleAlignmentSettings();
		VehicleAlignmentStatus = L"Failed to parse alignment JSON; using current/default values.";
		AppendCpuRuntimeTrace(L"[VehicleAlignment] parse failed: " + AnsiToWString(e.what()));
		return false;
	}
}

bool Corona::SaveVehicleAlignmentSettings()
{
	ClampVehicleAlignmentSettings();
	const std::filesystem::path path(GetVehicleAlignmentSettingsPath());
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	nlohmann::json root;
	root["version"] = 1;
	root["front_static_toe_deg"] = VehicleAlignment.FrontStaticToeDeg;
	root["rear_static_toe_deg"] = VehicleAlignment.RearStaticToeDeg;
	root["front_static_negative_camber_deg"] = VehicleAlignment.FrontStaticNegativeCamberDeg;
	root["rear_static_negative_camber_deg"] = VehicleAlignment.RearStaticNegativeCamberDeg;
	root["front_roll_dynamic_camber_deg"] = VehicleAlignment.FrontRollDynamicCamberDeg;
	root["rear_roll_dynamic_camber_deg"] = VehicleAlignment.RearRollDynamicCamberDeg;
	root["front_roll_toe_gain"] = VehicleAlignment.FrontRollToeGain;
	root["rear_roll_toe_gain"] = VehicleAlignment.RearRollToeGain;
	root["front_brake_toe_stabilize"] = VehicleAlignment.FrontBrakeToeStabilize;
	root["rear_brake_toe_stabilize"] = VehicleAlignment.RearBrakeToeStabilize;
	root["visual_alignment_scale"] = VehicleAlignment.VisualAlignmentScale;

	std::ofstream file(path, std::ios::trunc);
	if (!file.is_open())
	{
		VehicleAlignmentStatus = L"Failed to save alignment JSON: " + path.wstring();
		return false;
	}

	file << std::setw(2) << root << '\n';
	VehicleAlignmentStatus = L"Saved alignment JSON: " + path.wstring();
	AppendCpuRuntimeTrace(L"[VehicleAlignment] saved path=" + path.wstring());
	return true;
}

void Corona::InitializeVehicleDrivingMode()
{
	if (!bVehicleDrivingMode || VehicleDriving.bSpawned)
		return;

	LoadVehicleAlignmentSettings();

	if (!VehicleBodyScene)
		VehicleBodyScene = CreateProceduralBoxScene(glm::vec3(0.86f, 0.08f, 0.04f), false, 1.0f);
	if (!VehicleCabinScene)
		VehicleCabinScene = CreateProceduralBoxScene(glm::vec3(0.04f, 0.08f, 0.10f), false, 1.0f);
	if (!VehicleWheelScene)
		VehicleWheelScene = CreateProceduralCylinderScene(0.5f, 0.5f, 40u, glm::vec3(0.015f, 0.014f, 0.013f));
	if (!VehicleLoadBarScene)
		VehicleLoadBarScene = CreateProceduralBoxScene(glm::vec3(0.08f, 0.82f, 0.18f), false, 1.0f);
	if (!VehicleSlipPlaneScene)
		VehicleSlipPlaneScene = CreateProceduralBoxScene(glm::vec3(0.05f, 0.22f, 1.0f), false, 1.0f);
	auto addPart = [this](const char* name, const shared_ptr<Scene>& scene) -> SceneObjectHandle
	{
		SceneObjectDesc desc;
		desc.DebugName = name ? name : "VehiclePart";
		desc.ScenePtr = scene;
		desc.Transform = glm::mat4x4(1.0f);
		desc.Roughness = 0.72f;
		desc.Metallic = 0.0f;
		desc.bOverrideRoughnessMetallic = true;
		desc.bVisible = true;
		// Keep vehicle visuals out of the main static TLAS. Updating them through
		// the full city TLAS is much more expensive than rasterizing these parts.
		desc.bRayTracing = false;
		desc.bDynamicRayTracing = true;
		desc.bDynamicRaster = true;
		desc.bPhysicsQuery = false;
		desc.PhysicsCollisionShape = EPhysicsCollisionShape::Box;
		desc.PhysicsBoxHalfExtent = glm::vec3(0.5f);
		return AddSceneObject(desc);
	};

	VehicleDriving.BodyHandle = addPart("Vehicle_Body", VehicleBodyScene);
	VehicleDriving.CabinHandle = addPart("Vehicle_Cabin", VehicleCabinScene);
	VehicleDriving.WheelHandles = {
		addPart("Vehicle_Wheel_FL", VehicleWheelScene),
		addPart("Vehicle_Wheel_FR", VehicleWheelScene),
		addPart("Vehicle_Wheel_RL", VehicleWheelScene),
		addPart("Vehicle_Wheel_RR", VehicleWheelScene)
	};
	VehicleDriving.LoadBarHandles = {
		addPart("Vehicle_Load_FL", VehicleLoadBarScene),
		addPart("Vehicle_Load_FR", VehicleLoadBarScene),
		addPart("Vehicle_Load_RL", VehicleLoadBarScene),
		addPart("Vehicle_Load_RR", VehicleLoadBarScene)
	};
	VehicleDriving.SlipPlaneHandles = {
		addPart("Vehicle_Slip_FL", VehicleSlipPlaneScene),
		addPart("Vehicle_Slip_FR", VehicleSlipPlaneScene),
		addPart("Vehicle_Slip_RL", VehicleSlipPlaneScene),
		addPart("Vehicle_Slip_RR", VehicleSlipPlaneScene)
	};

	VehicleDriving.bSpawned = true;
	ResetVehicleDrivingMode();
	AppendCpuRuntimeTrace(L"[Vehicle] initialized four-wheel driving mode");
}

void Corona::ResetVehicleDrivingMode()
{
	VehicleDriving.Position = glm::vec3(0.0f, 120.0f, 0.0f);
	VehicleDriving.Yaw = 0.0f;
	float bestRoadScore = std::numeric_limits<float>::max();
	const RoadDecalState* bestRoad = nullptr;
	for (const RoadDecalState& road : RoadDecals)
	{
		if (!road.bEnabled || road.HalfLength <= std::max(road.HalfWidth * 1.45f, 24.0f))
			continue;
		const glm::vec2 centerXZ(road.Center.x, road.Center.z);
		const float centerDistanceSq = glm::dot(centerXZ, centerXZ);
		const float score = centerDistanceSq - std::min(road.HalfLength, 900.0f) * 160.0f;
		if (score < bestRoadScore)
		{
			bestRoadScore = score;
			bestRoad = &road;
		}
	}
	if (bestRoad)
	{
		VehicleDriving.Position = glm::vec3(bestRoad->Center.x, bestRoad->Height + 92.0f * kVehicleVisualScale, bestRoad->Center.z);
		glm::vec3 roadAxis(bestRoad->AxisX.x, 0.0f, bestRoad->AxisX.z);
		const float roadAxisLength = glm::length(roadAxis);
		if (roadAxisLength > 0.01f)
		{
			roadAxis /= roadAxisLength;
			VehicleDriving.Yaw = std::atan2(roadAxis.x, roadAxis.z);
		}
	}
	CpuPhysicsRaycastHit groundHit;
	if (CpuPhysicsRaycast(glm::vec3(VehicleDriving.Position.x, 5000.0f, VehicleDriving.Position.z), glm::vec3(0.0f, -1.0f, 0.0f), 12000.0f, groundHit))
		VehicleDriving.Position.y = groundHit.Position.y + 92.0f * kVehicleVisualScale;

	VehicleDriving.Velocity = glm::vec3(0.0f);
	VehicleDriving.YawRate = 0.0f;
	VehicleDriving.VerticalVelocity = 0.0f;
	VehicleDriving.Steering = 0.0f;
	VehicleDriving.Throttle = 0.0f;
	VehicleDriving.Brake = 0.0f;
	VehicleDriving.LiftOffOversteer = 0.0f;
	VehicleDriving.OversteerSustain = 0.0f;
	VehicleDriving.Gear = 1;
	VehicleDriving.EngineRpm = 900.0f;
	VehicleDriving.LongitudinalAccel = 0.0f;
	VehicleDriving.LateralAccel = 0.0f;
	VehicleDriving.VisualPitch = 0.0f;
	VehicleDriving.VisualRoll = 0.0f;
	VehicleDriving.WheelSpin = 0.0f;
	VehicleDriving.GroundedFraction = 1.0f;
	for (VehicleWheelState& wheel : VehicleDriving.Wheels)
	{
		wheel.Load = 1350.0f * 980.0f * 0.25f;
		wheel.Compression = 0.5f;
		wheel.CompressionVelocity = 0.0f;
		wheel.SlipRatio = 0.0f;
		wheel.SlipAngle = 0.0f;
		wheel.AngularVelocity = 0.0f;
		wheel.SpinAngle = 0.0f;
		wheel.DriveTorqueShare = 0.0f;
		wheel.bGrounded = true;
		wheel.ContactNormal = glm::vec3(0.0f, 1.0f, 0.0f);
	}
	VehicleDriving.bResetPending = false;
	UpdateVehicleVisuals();
}

void Corona::UpdateVehicleDrivingMode(float elapsedSeconds)
{
	if (!bVehicleDrivingMode)
		return;

	InitializeVehicleDrivingMode();
	if (!VehicleDriving.bSpawned)
		return;
	if (VehicleDriving.bResetPending || ScriptKeyPressed['R'])
		ResetVehicleDrivingMode();

	const float dt = std::clamp(elapsedSeconds, 0.0f, 1.0f / 30.0f);
	if (dt <= 0.0f)
		return;

	auto keyDown = [this](int key) -> bool
	{
		return key >= 0 && key < static_cast<int>(ScriptKeyDown.size()) && ScriptKeyDown[static_cast<size_t>(key)];
	};

	float steerInput =
		(keyDown('A') || keyDown(VK_LEFT) ? 1.0f : 0.0f) -
		(keyDown('D') || keyDown(VK_RIGHT) ? 1.0f : 0.0f);
	float forwardInput = (keyDown('W') || keyDown(VK_UP)) ? 1.0f : 0.0f;
	float reverseInput = (keyDown('S') || keyDown(VK_DOWN)) ? 1.0f : 0.0f;
	float brakeInput = keyDown(VK_SPACE) ? 1.0f : 0.0f;

	if (bScriptGamepadConnected)
	{
		steerInput = std::clamp(steerInput - ScriptGamepadLeftX, -1.0f, 1.0f);
		forwardInput = std::max(forwardInput, ScriptGamepadRightTrigger);
		brakeInput = std::max(brakeInput, ScriptGamepadLeftTrigger);
		if ((ScriptGamepadButtonsPressed & kVehicleGamepadButtonX) != 0)
			--VehicleDriving.Gear;
		if ((ScriptGamepadButtonsPressed & kVehicleGamepadButtonA) != 0)
			++VehicleDriving.Gear;
	}
	VehicleDriving.Gear = std::clamp(VehicleDriving.Gear, 0, 5);
	const bool bReverseGear = VehicleDriving.Gear == 0;

	const glm::vec3 forward = VehicleForwardFromYaw(VehicleDriving.Yaw);
	const glm::vec3 right = VehicleRightFromYaw(VehicleDriving.Yaw);
	const float speedForward = glm::dot(VehicleDriving.Velocity, forward);
	const float speed = glm::length(glm::vec2(VehicleDriving.Velocity.x, VehicleDriving.Velocity.z));
	if (reverseInput > 0.0f)
	{
		if (speedForward > 120.0f)
			brakeInput = std::max(brakeInput, reverseInput);
		else
			forwardInput -= 0.45f * reverseInput;
	}

	const float steeringResponse = 7.5f;
	const float targetSteering = std::clamp(steerInput, -1.0f, 1.0f);
	VehicleDriving.Steering += (targetSteering - VehicleDriving.Steering) *
		std::clamp(dt * steeringResponse, 0.0f, 1.0f);
	float driveInput = forwardInput;
	if (bReverseGear && driveInput > 0.0f)
		driveInput = -driveInput;
	const float previousThrottle = VehicleDriving.Throttle;
	VehicleDriving.Throttle = std::clamp(driveInput, -0.75f, 1.0f);
	VehicleDriving.Brake = std::clamp(brakeInput, 0.0f, 1.0f);
	const float previousPositiveThrottle = std::max(previousThrottle, 0.0f);
	const float currentPositiveThrottle = std::max(VehicleDriving.Throttle, 0.0f);
	const float throttleRise01 = std::clamp((currentPositiveThrottle - previousPositiveThrottle) * 3.50f, 0.0f, 1.0f);
	const float throttleOnImmediate01 =
		std::clamp(std::max((currentPositiveThrottle - 0.035f) / 0.34f, throttleRise01), 0.0f, 1.0f);
	const float positiveThrottleDrop01 = std::clamp((previousPositiveThrottle - currentPositiveThrottle) * 2.80f, 0.0f, 1.0f);
	const float partialThrottleLift01 =
		positiveThrottleDrop01 *
		std::clamp((previousPositiveThrottle - 0.10f) / 0.50f, 0.0f, 1.0f) *
		std::clamp((0.78f - currentPositiveThrottle) / 0.78f, 0.0f, 1.0f);
	const float throttleCut01 =
		std::clamp((previousPositiveThrottle - 0.18f) / 0.46f, 0.0f, 1.0f) *
		std::clamp((0.32f - currentPositiveThrottle) / 0.32f, 0.0f, 1.0f);
	const float liftOffTrigger01 = std::max(std::max(positiveThrottleDrop01, partialThrottleLift01), throttleCut01);
	VehicleDriving.LiftOffOversteer = std::clamp(
		std::max(
			VehicleDriving.LiftOffOversteer * std::pow(0.24f, dt),
			liftOffTrigger01) +
		partialThrottleLift01 * 0.22f,
		0.0f,
		1.0f);
	if (VehicleDriving.Throttle > 0.45f)
		VehicleDriving.LiftOffOversteer *= std::pow(0.025f, dt);
	else if (VehicleDriving.Throttle > 0.12f)
		VehicleDriving.LiftOffOversteer *= std::pow(0.24f, dt);
	VehicleDriving.LiftOffOversteer *= 1.0f - throttleOnImmediate01 * 0.94f;
	const float liftOffImmediate01 =
		std::clamp(
			std::max(liftOffTrigger01, VehicleDriving.LiftOffOversteer) *
			std::clamp((0.55f - currentPositiveThrottle) / 0.55f, 0.0f, 1.0f),
			0.0f,
			1.0f);

	constexpr float kMass = 1350.0f;
	constexpr float kGravity = 980.0f;
	constexpr float kWheelBase = 282.0f * kVehicleVisualScale;
	constexpr float kTrack = 164.0f * kVehicleVisualScale;
	constexpr float kCgHeight = 54.0f * kVehicleVisualScale;
	constexpr float kStaticFrontBias = 0.52f;
	constexpr float kTireMu = 1.18f;
	constexpr float kTireLoadSensitivity = 0.24f;
	constexpr float kDriveAccel = 690.0f;
	constexpr float kReverseAccel = 310.0f;
	constexpr float kBrakeAccel = 1050.0f;
	constexpr float kWheelPhysicsRadius = 28.0f;
	constexpr float kWheelRayStart = 70.0f * kVehicleVisualScale;
	constexpr float kWheelRayLength = 175.0f * kVehicleVisualScale;
	constexpr float kChassisRideHeight = 92.0f * kVehicleVisualScale;
	constexpr float kCollisionRadius = 72.0f * kVehicleVisualScale;
	constexpr float kCollisionSkin = 8.0f * kVehicleVisualScale;
	constexpr float kYawInertia = kMass * (kWheelBase * kWheelBase + kTrack * kTrack) / 12.0f * 2.15f;
	constexpr float kFrontDriveTorqueShare = 0.50f;
	constexpr float kRearDriveTorqueShare = 0.50f;
	constexpr float kIdleEngineRpm = 900.0f;
	constexpr float kMaxEngineRpm = 7200.0f;
	constexpr float kGlobalGearSpeedScale = 0.125f;
	constexpr std::array<float, 5> kGearTorqueScales = { 2.20f, 1.35f, 1.00f, 0.76f, 0.58f };
	constexpr std::array<float, 5> kGearSpeedLimits = { 2450.0f, 4850.0f, 7200.0f, 9800.0f, 12800.0f };

	const glm::vec3 oldVelocity = VehicleDriving.Velocity;
	const glm::vec3 oldPosition = VehicleDriving.Position;

	auto smooth01 = [](float edge0, float edge1, float value) -> float
	{
		const float t = std::clamp((value - edge0) / std::max(edge1 - edge0, 0.0001f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	auto tireFrictionCurve = [](float normalizedRequest, float slidingFrictionScale, float falloff) -> float
	{
		const float x = std::max(normalizedRequest, 0.0f);
		if (x <= 1.0f)
			return std::sin(x * glm::half_pi<float>());
		const float t = std::clamp(1.0f - std::exp(-(x - 1.0f) * falloff), 0.0f, 1.0f);
		return glm::mix(1.0f, slidingFrictionScale, t);
	};

	const float totalStaticLoad = kMass * kGravity;
	const float nominalWheelLoad = totalStaticLoad * 0.25f;
	const int gearIndex = std::clamp(VehicleDriving.Gear - 1, 0, 4);
	const float gearSpeedLimit = kGearSpeedLimits[static_cast<size_t>(gearIndex)] * kGlobalGearSpeedScale;
	constexpr float kReverseSpeedLimit = 1700.0f * kGlobalGearSpeedScale;
	const float forwardSpeedRatio = std::clamp(std::max(speedForward, 0.0f) / std::max(gearSpeedLimit, 1.0f), 0.0f, 1.18f);
	const float reverseSpeedRatio = std::clamp(std::max(-speedForward, 0.0f) / kReverseSpeedLimit, 0.0f, 1.18f);
	const float targetEngineRpm =
		VehicleDriving.Throttle < 0.0f || bReverseGear ?
		kIdleEngineRpm + reverseSpeedRatio * (kMaxEngineRpm - kIdleEngineRpm) :
		kIdleEngineRpm + forwardSpeedRatio * (kMaxEngineRpm - kIdleEngineRpm);
	VehicleDriving.EngineRpm += (targetEngineRpm - VehicleDriving.EngineRpm) * std::clamp(dt * 12.0f, 0.0f, 1.0f);
	VehicleDriving.EngineRpm = std::clamp(VehicleDriving.EngineRpm, kIdleEngineRpm, kMaxEngineRpm * 1.12f);
	const float engineRpm01 = std::clamp((VehicleDriving.EngineRpm - kIdleEngineRpm) / (kMaxEngineRpm - kIdleEngineRpm), 0.0f, 1.12f);
	const float torqueCurve =
		std::clamp(0.82f + std::sin(std::clamp(engineRpm01, 0.0f, 1.0f) * glm::pi<float>()) * 0.28f, 0.62f, 1.10f);
	const float redlineCut01 = smooth01(kMaxEngineRpm * 0.985f, kMaxEngineRpm * 1.045f, VehicleDriving.EngineRpm);
	const float forwardGearDriveScale =
		kGearTorqueScales[static_cast<size_t>(gearIndex)] * torqueCurve * (1.0f - redlineCut01);
	const float reverseGearDriveScale = 0.92f * torqueCurve * (1.0f - redlineCut01);
	const float activeDriveScale = VehicleDriving.Throttle >= 0.0f ? forwardGearDriveScale : reverseGearDriveScale;
	float predictedLongitudinalAccel =
		VehicleDriving.Throttle >= 0.0f ? VehicleDriving.Throttle * kDriveAccel * activeDriveScale : VehicleDriving.Throttle * kReverseAccel * activeDriveScale;
	if (VehicleDriving.Brake > 0.0f && std::abs(speedForward) > 1.0f)
		predictedLongitudinalAccel -= VehicleSignNonZero(speedForward) * VehicleDriving.Brake * kBrakeAccel;
	const float throttleTorque01 = smooth01(0.08f, 1.0f, std::max(VehicleDriving.Throttle, 0.0f));
	const float highSpeedStability01 = smooth01(6200.0f, 9800.0f, speed);
	const float highGear01 = smooth01(2.5f, 4.0f, static_cast<float>(VehicleDriving.Gear));
	const float highGearLiftOffSteerRelief01 =
		std::clamp(
			VehicleDriving.LiftOffOversteer *
			highGear01 *
			smooth01(3000.0f, 8200.0f, speed),
			0.0f,
			1.0f);
	const float highSpeedSteerPenalty =
		highSpeedStability01 * std::max(0.10f, 0.35f - highGearLiftOffSteerRelief01 * 0.26f);
	const float maxSteerAngle =
		glm::radians(34.0f) / (1.0f + speed * 0.00014f + highSpeedSteerPenalty);
	float predictedLateralAccel = 0.0f;
	if (std::abs(VehicleDriving.Steering) > 0.001f && speed > 1.0f)
		predictedLateralAccel = speed * speed * std::tan(VehicleDriving.Steering * maxSteerAngle) / std::max(kWheelBase, 1.0f);
	const float steerActivity01 = smooth01(0.05f, 0.46f, std::abs(VehicleDriving.Steering));
	const float hardBrakeTurnGuard01 =
		smooth01(0.46f, 0.92f, VehicleDriving.Brake) *
		smooth01(2400.0f, 7200.0f, speed) *
		steerActivity01;
	const float liftOffLoadTransfer01 =
		std::max(VehicleDriving.LiftOffOversteer, liftOffImmediate01 * 1.12f) *
		smooth01(1400.0f, 7200.0f, speed) *
		(1.0f - VehicleDriving.Brake * 0.65f) *
		(1.0f - throttleOnImmediate01 * 0.96f);
	const float liftOffLongitudinalDecel = kBrakeAccel * 0.94f * liftOffLoadTransfer01;
	const float retainedPositiveLongAccelScale =
		1.0f - std::clamp(liftOffImmediate01 * smooth01(700.0f, 4200.0f, speed), 0.0f, 1.0f);
	const float retainedNegativeLongAccelScale =
		1.0f - std::clamp(throttleOnImmediate01 * smooth01(500.0f, 3800.0f, speed), 0.0f, 1.0f);
	const float retainedLongitudinalAccel =
		VehicleDriving.LongitudinalAccel > 0.0f ?
		VehicleDriving.LongitudinalAccel * retainedPositiveLongAccelScale :
		VehicleDriving.LongitudinalAccel * retainedNegativeLongAccelScale;
	const float predictedLongitudinalAccelForLoad =
		predictedLongitudinalAccel > 0.0f ?
		predictedLongitudinalAccel * retainedPositiveLongAccelScale :
		predictedLongitudinalAccel;
	const float loadTransferLongAccel = std::clamp(
		retainedLongitudinalAccel * 0.38f + predictedLongitudinalAccelForLoad * 0.62f - liftOffLongitudinalDecel,
		-kBrakeAccel * 1.25f,
		kDriveAccel * 1.25f);
	const float lateralAccelForLoadTransfer =
		VehicleDriving.LateralAccel * (0.55f - hardBrakeTurnGuard01 * 0.20f) +
		predictedLateralAccel * (0.45f - hardBrakeTurnGuard01 * 0.38f);
	const float loadTransferLatAccel = std::clamp(
		lateralAccelForLoadTransfer * (1.0f - hardBrakeTurnGuard01 * 0.24f),
		-kGravity * 1.4f,
		kGravity * 1.4f);
	const float physicsRollForAlignment = std::clamp(loadTransferLatAccel / 3600.0f, -0.030f, 0.030f);
	const float brakeLoadTransferBoost01 =
		smooth01(0.28f, 0.95f, VehicleDriving.Brake) *
		smooth01(1600.0f, 7200.0f, speed);
	const float effectiveLongAccelForLoadTransfer =
		loadTransferLongAccel < 0.0f ?
		loadTransferLongAccel * (1.0f + brakeLoadTransferBoost01 * 0.44f) :
		loadTransferLongAccel;
	float frontAxleLoad = totalStaticLoad * kStaticFrontBias -
		kMass * effectiveLongAccelForLoadTransfer * kCgHeight / kWheelBase;
	frontAxleLoad = std::clamp(frontAxleLoad, totalStaticLoad * 0.18f, totalStaticLoad * 0.88f);
	const float rearAxleLoad = totalStaticLoad - frontAxleLoad;
	const float frontRearLoadDelta = (frontAxleLoad - rearAxleLoad) / std::max(totalStaticLoad, 1.0f);
	const float hardBrakeLongTransfer01 =
		smooth01(0.34f, 0.95f, VehicleDriving.Brake) *
		smooth01(1800.0f, 7200.0f, speed) *
		smooth01(0.04f, 0.34f, frontRearLoadDelta);
	const float liftOffForwardTransfer01 =
		std::clamp(
			liftOffLoadTransfer01 *
			smooth01(0.04f, 0.34f, frontRearLoadDelta) *
			(0.45f + steerActivity01 * 0.55f),
			0.0f,
			1.0f);
	const float forwardLoadTransfer01 = std::max(hardBrakeLongTransfer01, liftOffForwardTransfer01);
	const float lateralTransferLimit =
		totalStaticLoad *
		(0.35f - hardBrakeTurnGuard01 * 0.13f - forwardLoadTransfer01 * 0.12f);
	const float lateralTransfer = std::clamp(
		kMass * loadTransferLatAccel * kCgHeight / kTrack,
		-lateralTransferLimit,
		lateralTransferLimit);
	const float frontAxleShareForRoll = std::clamp(frontAxleLoad / std::max(totalStaticLoad, 1.0f), 0.18f, 0.88f);
	const float frontRollDistribution =
		std::clamp(
			frontAxleShareForRoll +
			liftOffForwardTransfer01 * 0.18f +
			hardBrakeLongTransfer01 * 0.10f,
			0.42f,
			0.88f);
	const float rollTransferScale = 1.0f - hardBrakeTurnGuard01 * 0.16f;
	const float frontLateralShareForRoll = frontRollDistribution * rollTransferScale;
	const float rearLateralShareForRoll = (1.0f - frontRollDistribution) * rollTransferScale;
	const float frontHeavyLoad01 = smooth01(0.04f, 0.28f, frontRearLoadDelta);
	const float rearHeavyLoad01 = smooth01(0.04f, 0.28f, -frontRearLoadDelta);
	const float speedBalance01 = smooth01(1200.0f, 5200.0f, speed);
	const float hardBrakeLock01 =
		smooth01(0.68f, 0.96f, VehicleDriving.Brake) *
		smooth01(1600.0f, 6200.0f, speed) *
		(0.42f + steerActivity01 * 0.58f);
	const float brakingStability01 =
		std::clamp(
			hardBrakeLock01 +
			VehicleDriving.Brake * smooth01(2600.0f, 7600.0f, speed) * (0.10f + std::abs(VehicleDriving.Steering) * 0.18f),
			0.0f,
			1.0f);
	const float brakePressureCurve01 =
		VehicleDriving.Brake * (0.22f + VehicleDriving.Brake * 0.78f);
	const float trailBrakeWindow01 =
		smooth01(0.035f, 0.28f, VehicleDriving.Brake) *
		(1.0f - smooth01(0.58f, 0.84f, VehicleDriving.Brake));
	const float lightBrakeTurnIn01 =
		trailBrakeWindow01 *
		speedBalance01 *
		(0.36f + steerActivity01 * 0.82f) *
		(0.58f + frontHeavyLoad01 * 0.58f) *
		(1.0f - hardBrakeLock01 * 0.80f);
	const float midBrakeRotation01 =
		std::clamp(
			lightBrakeTurnIn01 *
			(0.68f + frontHeavyLoad01 * 0.52f) *
			(1.0f - hardBrakeLock01 * 0.88f),
			0.0f,
			1.0f);
	const float liftOffImmediateRotation01 =
		std::clamp(
			liftOffImmediate01 *
			speedBalance01 *
			(0.62f + steerActivity01 * 0.72f) *
			(1.0f - VehicleDriving.Brake * 0.42f),
			0.0f,
			1.0f);
	const float liftOffOversteer01 =
		std::clamp(
			std::max(
				VehicleDriving.LiftOffOversteer *
				1.45f *
				speedBalance01 *
				(0.48f + steerActivity01 * 0.62f) *
				(1.0f - VehicleDriving.Brake * 0.55f),
				liftOffImmediateRotation01 * 1.18f),
			0.0f,
			1.0f);
	const float highGearLiftOff01 =
		std::clamp(
			liftOffOversteer01 *
			highGear01 *
			smooth01(3200.0f, 7600.0f, speed) *
			(0.58f + steerActivity01 * 0.42f) *
			(1.0f - throttleOnImmediate01 * 0.96f),
			0.0f,
			1.0f);
	const float liftRotationDemand01 =
		std::clamp(
			std::max(liftOffOversteer01, liftOffImmediateRotation01 * 1.12f) *
			(1.0f - throttleOnImmediate01 * 0.96f),
			0.0f,
			1.0f);
	const float throttleUndersteerImmediate01 =
		std::clamp(
			throttleOnImmediate01 *
			smooth01(600.0f, 4200.0f, speed) *
			(0.42f + steerActivity01 * 0.58f) *
			(0.48f + speedBalance01 * 0.52f),
			0.0f,
			1.0f);
	const float positiveAccel01 = smooth01(80.0f, kDriveAccel * 0.85f, std::max(loadTransferLongAccel, 0.0f));
	constexpr float kAccelerationUndersteerScale = 5.5f;
	const bool bLowForwardGear = VehicleDriving.Gear == 1 || VehicleDriving.Gear == 2;
	const float lowGearLaunchSlipPotential01 =
		bLowForwardGear ?
		std::clamp(smooth01(0.68f, 1.0f, VehicleDriving.Throttle) * (1.0f - redlineCut01 * 0.58f), 0.0f, 1.0f) :
		0.0f;
	const float powerUndersteer01 =
		std::clamp(
			std::max(
				positiveAccel01 * (0.35f + rearHeavyLoad01 * 0.65f) * (0.45f + speedBalance01 * 0.55f) * kAccelerationUndersteerScale,
				throttleUndersteerImmediate01 * 0.92f),
			0.0f,
			1.0f);
	const float hardPowerUndersteer01 =
		std::clamp(
			smooth01(kDriveAccel * 0.44f, kDriveAccel * 1.04f, std::max(loadTransferLongAccel, 0.0f)) *
			throttleTorque01 *
			(0.42f + speedBalance01 * 0.58f) *
			(1.0f - lowGearLaunchSlipPotential01 * 0.66f),
			0.0f,
			1.0f) *
		kAccelerationUndersteerScale;
	const float hardPowerUndersteerClamped01 =
		std::clamp(std::max(hardPowerUndersteer01, throttleUndersteerImmediate01 * 0.86f), 0.0f, 1.0f);
	const float highSpeedPowerUndersteer01 =
		std::clamp(
			std::max(
				throttleTorque01 *
				smooth01(1800.0f, 6200.0f, speed) *
				(0.35f + positiveAccel01 * 0.65f) *
				(0.46f + highSpeedStability01 * 0.54f) *
				(0.48f + steerActivity01 * 0.52f) *
				(1.0f - lowGearLaunchSlipPotential01 * 0.38f) *
				2.65f,
				throttleUndersteerImmediate01 * 0.72f),
			0.0f,
			1.0f);
	const float brakeTurnGuard01 =
		hardBrakeTurnGuard01 * (0.35f + brakingStability01 * 0.65f);
	const float brakeOversteer01 =
		std::clamp(
			frontHeavyLoad01 *
			speedBalance01 *
			(0.08f + midBrakeRotation01 * 1.10f + VehicleDriving.Brake * 0.10f) *
			(1.0f - hardBrakeLock01 * 0.86f),
			0.0f,
			1.0f);
	const float accelerationUndersteer01 =
		std::clamp(std::max(std::max(powerUndersteer01, hardPowerUndersteerClamped01), highSpeedPowerUndersteer01), 0.0f, 1.0f);
	const float preYawRate01 = smooth01(0.10f, 0.95f, std::abs(VehicleDriving.YawRate));
	const float oversteerExcite01 = std::clamp(
		liftOffOversteer01 * 0.74f +
		midBrakeRotation01 * 0.10f +
		brakeOversteer01 * 0.06f,
		0.0f,
		1.0f);
	(void)oversteerExcite01;
	VehicleDriving.OversteerSustain *= std::pow(0.02f, dt);
	const float oversteerSustain01 = 0.0f;

	constexpr std::array<float, 2> kOpenAxleDiffShares = { 0.5f, 0.5f };
	const auto frontDiffShares = kOpenAxleDiffShares;
	const auto rearDiffShares = kOpenAxleDiffShares;
	const float frontDriveShare = kFrontDriveTorqueShare;
	const float rearDriveShare = kRearDriveTorqueShare;
	std::array<float, 4> wheelDriveTorqueShares = {};
	wheelDriveTorqueShares[kVehicleWheelFL] = frontDriveShare * frontDiffShares[0];
	wheelDriveTorqueShares[kVehicleWheelFR] = frontDriveShare * frontDiffShares[1];
	wheelDriveTorqueShares[kVehicleWheelRL] = rearDriveShare * rearDiffShares[0];
	wheelDriveTorqueShares[kVehicleWheelRR] = rearDriveShare * rearDiffShares[1];

	const std::array<glm::vec3, 4> wheelLocal = {
		glm::vec3(-kTrack * 0.5f, -44.0f * kVehicleVisualScale,  kWheelBase * 0.5f),
		glm::vec3( kTrack * 0.5f, -44.0f * kVehicleVisualScale,  kWheelBase * 0.5f),
		glm::vec3(-kTrack * 0.5f, -44.0f * kVehicleVisualScale, -kWheelBase * 0.5f),
		glm::vec3( kTrack * 0.5f, -44.0f * kVehicleVisualScale, -kWheelBase * 0.5f)
	};

	glm::vec3 totalForce(0.0f);
	float totalYawTorque = 0.0f;
	float groundHeightSum = 0.0f;
	float rearPowerSlipMax = 0.0f;
	int groundedWheels = 0;

	for (int i = 0; i < 4; ++i)
	{
		const bool bFront = i == kVehicleWheelFL || i == kVehicleWheelFR;
		const bool bRight = i == kVehicleWheelFR || i == kVehicleWheelRR;
		const float axleLoad = bFront ? frontAxleLoad : rearAxleLoad;
		const float axleLateralShare = bFront ? frontLateralShareForRoll : rearLateralShareForRoll;
		const float maxAxleLateralDelta =
			axleLoad *
			(bFront ?
				std::clamp(0.40f + forwardLoadTransfer01 * 0.12f, 0.35f, 0.56f) :
				std::clamp(0.24f - forwardLoadTransfer01 * 0.18f, 0.06f, 0.26f));
		const float lateralWheelDelta =
			std::clamp(lateralTransfer * axleLateralShare, -maxAxleLateralDelta, maxAxleLateralDelta);
		float wheelLoad = axleLoad * 0.5f - (bRight ? 1.0f : -1.0f) * lateralWheelDelta;
		const float minWheelLoad = totalStaticLoad * (0.04f + hardBrakeTurnGuard01 * 0.035f);
		const float maxWheelLoad =
			std::max(
				minWheelLoad + 1.0f,
				std::min(
					totalStaticLoad * (0.55f - hardBrakeTurnGuard01 * 0.075f),
					axleLoad *
					(bFront ?
						std::clamp(0.78f + forwardLoadTransfer01 * 0.06f, 0.78f, 0.84f) :
						std::clamp(0.62f - forwardLoadTransfer01 * 0.30f, 0.30f, 0.64f))));
		wheelLoad = std::clamp(wheelLoad, minWheelLoad, maxWheelLoad);

		VehicleWheelState& wheel = VehicleDriving.Wheels[static_cast<size_t>(i)];
		const float previousCompression = wheel.Compression;
		const float previousLoad = wheel.Load;
		const bool bWasGrounded = wheel.bGrounded;
		wheel.Load = 0.0f;
		wheel.bGrounded = false;
		wheel.Compression = 0.0f;
		wheel.CompressionVelocity = 0.0f;
		wheel.SlipRatio = 0.0f;
		wheel.SlipAngle = 0.0f;
		const float driveTorqueShare = wheelDriveTorqueShares[static_cast<size_t>(i)];
		wheel.DriveTorqueShare = driveTorqueShare;

		const glm::vec3 hardPoint = VehicleDriving.Position + right * wheelLocal[i].x + forward * wheelLocal[i].z;
		CpuPhysicsRaycastHit hit;
		if (!CpuPhysicsRaycast(hardPoint + glm::vec3(0.0f, kWheelRayStart, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), kWheelRayLength, hit) ||
			hit.Normal.y < 0.35f)
		{
			const float freeWheelAccel =
				(VehicleDriving.Throttle >= 0.0f ? VehicleDriving.Throttle * kDriveAccel * activeDriveScale : VehicleDriving.Throttle * kReverseAccel * activeDriveScale) *
				driveTorqueShare / kWheelPhysicsRadius;
			wheel.AngularVelocity += freeWheelAccel * dt * 4.0f;
			wheel.AngularVelocity *= std::pow(0.82f, dt);
			wheel.SpinAngle += wheel.AngularVelocity * dt;
			continue;
		}

		wheel.bGrounded = true;
		wheel.ContactPoint = hit.Position;
		wheel.ContactNormal = hit.Normal;
		wheel.Compression = std::clamp((kWheelRayLength - hit.Distance) / kWheelRayLength, 0.0f, 1.0f);
		wheel.CompressionVelocity = std::clamp((wheel.Compression - previousCompression) / std::max(dt, 0.0001f), -5.0f, 5.0f);
		const float suspensionScale = std::clamp(
			0.84f + wheel.Compression * 0.26f + wheel.CompressionVelocity * 0.004f,
			0.68f,
			1.24f);
		const float targetWheelLoad = std::clamp(
			wheelLoad * suspensionScale,
			totalStaticLoad * 0.02f,
			totalStaticLoad * 0.62f);
		wheel.Load =
			bWasGrounded && previousLoad > 0.0f ?
			glm::mix(previousLoad, targetWheelLoad, std::clamp(dt * 28.0f, 0.0f, 1.0f)) :
			targetWheelLoad;
		groundHeightSum += hit.Position.y;
		++groundedWheels;

		const float sideSign = bRight ? 1.0f : -1.0f;
		const float baseSteerAngle = bFront ? VehicleDriving.Steering * maxSteerAngle : 0.0f;
		const float staticToeAngle =
			bFront ?
			sideSign * glm::radians(VehicleAlignment.FrontStaticToeDeg) :
			-sideSign * glm::radians(VehicleAlignment.RearStaticToeDeg);
		const float rollToeAngle =
			(bFront ? VehicleAlignment.FrontRollToeGain : VehicleAlignment.RearRollToeGain) * physicsRollForAlignment * sideSign;
		const float brakeToeStabilizeAngle =
			bFront ?
			-baseSteerAngle * brakingStability01 * VehicleAlignment.FrontBrakeToeStabilize :
			-VehicleDriving.YawRate * brakingStability01 * VehicleAlignment.RearBrakeToeStabilize;
		const float steerAngle = baseSteerAngle + staticToeAngle + rollToeAngle + brakeToeStabilizeAngle;
		const float rollMagnitude01 = smooth01(0.004f, 0.030f, std::abs(physicsRollForAlignment));
		const float loadedWheel01 = std::clamp((wheel.Load / std::max(nominalWheelLoad, 1.0f) - 1.0f) / 0.75f, 0.0f, 1.0f);
		const float staticNegativeCamber = glm::radians(bFront ? VehicleAlignment.FrontStaticNegativeCamberDeg : VehicleAlignment.RearStaticNegativeCamberDeg);
		const float dynamicNegativeCamber =
			glm::radians(bFront ? VehicleAlignment.FrontRollDynamicCamberDeg : VehicleAlignment.RearRollDynamicCamberDeg) *
			rollMagnitude01 *
			(0.35f + loadedWheel01 * 0.65f);
		const float camberAngle = -(staticNegativeCamber + dynamicNegativeCamber);
		const float camberGripScale = std::clamp(
			1.0f +
			rollMagnitude01 * (0.04f + loadedWheel01 * 0.14f) +
			std::clamp(dynamicNegativeCamber / glm::radians(2.60f), 0.0f, 1.0f) * 0.08f,
			0.94f,
			1.24f);
		const glm::vec3 wheelFwd = glm::normalize(forward * std::cos(steerAngle) + right * std::sin(steerAngle));
		const glm::vec3 wheelRight = glm::normalize(right * std::cos(steerAngle) - forward * std::sin(steerAngle));
		const glm::vec3 r = right * wheelLocal[i].x + forward * wheelLocal[i].z;
		const glm::vec3 yawVelocity(VehicleDriving.YawRate * r.z, 0.0f, -VehicleDriving.YawRate * r.x);
		const glm::vec3 contactVelocity = VehicleDriving.Velocity + yawVelocity;
		const float vLong = glm::dot(contactVelocity, wheelFwd);
		const float vLat = glm::dot(contactVelocity, wheelRight);

		float driveLongForce = 0.0f;
		float engineBrakeLongForce = 0.0f;
		float brakeDemandForce = 0.0f;
		if (VehicleDriving.Throttle != 0.0f)
		{
			const float driveAccel = VehicleDriving.Throttle >= 0.0f ? kDriveAccel * activeDriveScale : kReverseAccel * activeDriveScale;
			driveLongForce += VehicleDriving.Throttle * kMass * driveAccel * driveTorqueShare;
		}
		if (VehicleDriving.Throttle < 0.08f && speed > 250.0f && vLong > 1.0f)
		{
			const float engineBrake01 =
				liftOffImmediate01 *
				smooth01(200.0f, 2800.0f, speed) *
				std::clamp((0.08f - VehicleDriving.Throttle) / 0.08f, 0.0f, 1.0f);
			const float rearEngineBrakeBias = bFront ? 0.36f : 0.64f;
			engineBrakeLongForce -= engineBrake01 * kMass * kBrakeAccel * 0.078f * rearEngineBrakeBias;
		}
		const float brakePressure01 = brakePressureCurve01;
		const float brakeReferenceLongSpeed =
			bFront ? glm::mix(vLong, glm::dot(contactVelocity, forward), hardBrakeTurnGuard01) : vLong;
		if (VehicleDriving.Brake > 0.0f && std::abs(brakeReferenceLongSpeed) > 1.0f)
			brakeDemandForce += brakePressure01 * kMass * kBrakeAccel * (bFront ? 0.35f : 0.15f);
		const float rollingLongForce = -vLong * kMass * 0.12f;
		const float brakeSignSource = std::abs(vLong) > 1.0f ? vLong : speedForward;
		const float brakeLongForce = -VehicleSignNonZero(brakeSignSource) * brakeDemandForce;
		const float desiredLongForce = driveLongForce + engineBrakeLongForce + rollingLongForce + brakeLongForce;

		const float loadRatio = std::clamp(wheel.Load / std::max(nominalWheelLoad, 1.0f), 0.05f, 2.5f);
		const float loadSensitiveMu = std::clamp(
			kTireMu * (1.0f - kTireLoadSensitivity * (loadRatio - 1.0f)),
			kTireMu * 0.62f,
			kTireMu * 1.08f);
		const float unloadedGripScale =
			loadRatio < 1.0f ?
			std::clamp(0.16f + 0.84f * std::pow(loadRatio, 1.70f), 0.12f, 1.0f) :
			1.0f;
		const float grip = loadSensitiveMu * wheel.Load * unloadedGripScale;
		const float driveDemand01 = std::clamp(std::abs(desiredLongForce) / std::max(grip, 1.0f), 0.0f, 2.0f);
		const float rearPowerSlip01 =
			0.0f;
		rearPowerSlipMax = std::max(rearPowerSlipMax, rearPowerSlip01);
		float corneringStiffnessScale = 1.0f;
		float lateralGripScale = 1.0f;
		float longitudinalGripScale = 1.0f;
		if (bFront)
		{
			const float frontBrakeGripReserve01 = std::max(brakingStability01, hardBrakeTurnGuard01);
			const float highSpeedFrontPenalty01 =
				highSpeedStability01 *
				(1.0f - frontBrakeGripReserve01) *
				(1.0f - std::clamp(highGearLiftOff01 * 0.78f + liftRotationDemand01 * 0.30f, 0.0f, 0.92f));
			corneringStiffnessScale *= 1.0f - powerUndersteer01 * 0.72f - hardPowerUndersteerClamped01 * 0.96f - highSpeedPowerUndersteer01 * 0.94f - highSpeedFrontPenalty01 * 0.14f;
			lateralGripScale *= 1.0f - powerUndersteer01 * 0.62f - hardPowerUndersteerClamped01 * 0.98f - highSpeedPowerUndersteer01 * 0.92f - rearHeavyLoad01 * 0.12f - highSpeedFrontPenalty01 * 0.08f;
			corneringStiffnessScale *= 1.0f + brakingStability01 * 0.14f;
			lateralGripScale *= 1.0f + brakingStability01 * 0.20f;
			corneringStiffnessScale *= 1.0f + midBrakeRotation01 * 0.18f;
			lateralGripScale *= 1.0f + midBrakeRotation01 * 0.20f;
			corneringStiffnessScale *= 1.0f + liftRotationDemand01 * 0.42f;
			lateralGripScale *= 1.0f + liftRotationDemand01 * 0.36f;
			corneringStiffnessScale *= 1.0f + highGearLiftOff01 * 0.48f;
			lateralGripScale *= 1.0f + highGearLiftOff01 * 0.38f;
			corneringStiffnessScale *= 1.0f + oversteerSustain01 * 0.14f;
			lateralGripScale *= 1.0f + oversteerSustain01 * 0.12f;
			corneringStiffnessScale *= 1.0f + hardBrakeTurnGuard01 * 0.24f;
			lateralGripScale *= 1.0f + hardBrakeTurnGuard01 * 0.30f;
			corneringStiffnessScale *= 1.0f - hardBrakeLock01 * 0.28f;
			lateralGripScale *= 1.0f - hardBrakeLock01 * 0.42f;
			longitudinalGripScale *= 1.0f - hardBrakeTurnGuard01 * 0.28f - hardBrakeLock01 * 0.24f;
		}
		else
		{
			corneringStiffnessScale *= 1.0f - brakeOversteer01 * 0.16f - rearPowerSlip01 * 0.62f - midBrakeRotation01 * 0.34f - liftRotationDemand01 * 0.55f - highGearLiftOff01 * 0.36f;
			lateralGripScale *= 1.0f + powerUndersteer01 * 0.12f + highSpeedStability01 * 0.05f - brakeOversteer01 * 0.24f - rearPowerSlip01 * 0.78f - midBrakeRotation01 * 0.46f - liftRotationDemand01 * 0.58f - highGearLiftOff01 * 0.40f;
			corneringStiffnessScale *= 1.0f + highSpeedPowerUndersteer01 * 0.16f;
			lateralGripScale *= 1.0f + highSpeedPowerUndersteer01 * 0.34f;
			longitudinalGripScale *= 1.0f - rearPowerSlip01 * 0.42f;
			longitudinalGripScale *= 1.0f - liftRotationDemand01 * 0.22f;
			longitudinalGripScale *= 1.0f - highGearLiftOff01 * 0.10f;
			lateralGripScale *= 1.0f + hardBrakeLock01 * 0.24f;
		}
		const float combinedSlip01 = std::clamp((std::abs(desiredLongForce) + brakeDemandForce) / std::max(grip, 1.0f), 0.0f, 1.0f);
		if (!bFront)
			lateralGripScale *= 1.0f - VehicleDriving.Brake * speedBalance01 * combinedSlip01 * (0.070f + midBrakeRotation01 * 0.22f);
		const float frontBrakeTurnRetention01 = std::max(brakingStability01, hardBrakeTurnGuard01) * smooth01(0.04f, 0.46f, std::abs(VehicleDriving.Steering));
		const float frontPowerWashoutMin01 = highSpeedPowerUndersteer01 * (1.0f - frontBrakeTurnRetention01);
		const float minFrontLateralGripScale = 0.035f - frontPowerWashoutMin01 * 0.028f + frontBrakeTurnRetention01 * 0.75f;
		const float minFrontCorneringStiffnessScale = 0.040f - frontPowerWashoutMin01 * 0.032f + frontBrakeTurnRetention01 * 0.78f;
		const float rearMinLateralGripScale = 0.34f - midBrakeRotation01 * 0.08f - liftRotationDemand01 * 0.12f - highGearLiftOff01 * 0.06f + hardBrakeLock01 * 0.08f;
		const float rearMinCorneringStiffnessScale = 0.34f - midBrakeRotation01 * 0.08f - liftRotationDemand01 * 0.12f - highGearLiftOff01 * 0.06f + hardBrakeLock01 * 0.08f;
		lateralGripScale = std::clamp(lateralGripScale, bFront ? minFrontLateralGripScale : rearMinLateralGripScale, bFront ? 1.58f : 1.16f);
		corneringStiffnessScale = std::clamp(corneringStiffnessScale, bFront ? minFrontCorneringStiffnessScale : rearMinCorneringStiffnessScale, bFront ? 1.56f : 1.16f);
		longitudinalGripScale = std::clamp(longitudinalGripScale, 0.44f, 1.0f);
		const float lateralGrip = grip * lateralGripScale * camberGripScale;
		const float longitudinalGrip = grip * longitudinalGripScale;
		const float tireSlipAngle = std::atan2(vLat, std::abs(vLong) + 120.0f);
		const float corneringForcePerLoad = bFront ? 7.2f : (7.2f + highSpeedStability01 * 1.3f);
		float powerLimitedLateralGrip = lateralGrip;
		if (bFront && driveLongForce > 0.0f)
		{
			const float frontDriveDemand01 =
				std::clamp(driveLongForce / std::max(longitudinalGrip * 0.42f, 1.0f), 0.0f, 1.0f);
			const float frontPowerLateralLoss01 =
				std::clamp(
					frontDriveDemand01 *
					(0.58f + highSpeedPowerUndersteer01 * 0.42f) *
					smooth01(1100.0f, 5400.0f, speed),
					0.0f,
					1.0f);
			const float frontDriveReservedGrip =
				std::min(
					std::abs(driveLongForce) * (0.62f + highSpeedPowerUndersteer01 * 0.58f),
					longitudinalGrip * 0.98f);
			const float frontDriveLateralBudget =
				std::sqrt(std::max(0.0f, powerLimitedLateralGrip * powerLimitedLateralGrip - frontDriveReservedGrip * frontDriveReservedGrip));
			powerLimitedLateralGrip =
				std::min(
					powerLimitedLateralGrip,
					std::max(lateralGrip * 0.055f, frontDriveLateralBudget));
			const float lateralCapacityScale = std::clamp(1.0f - frontPowerLateralLoss01 * 0.88f, 0.08f, 1.0f);
			powerLimitedLateralGrip *= lateralCapacityScale;
		}
		const float rawLateralForce = -tireSlipAngle * wheel.Load * corneringForcePerLoad * corneringStiffnessScale;
		const float trailBrake01 = std::clamp(midBrakeRotation01 * (0.75f + frontHeavyLoad01 * 0.55f), 0.0f, 1.0f);
		const float liftRotation01 = std::clamp(liftRotationDemand01 * 1.20f + highGearLiftOff01 * 0.78f, 0.0f, 1.0f);
		const float lateralCurveRequestScale =
			bFront ?
			std::clamp(1.0f - trailBrake01 * 0.24f - liftRotation01 * 0.06f + accelerationUndersteer01 * 0.08f + hardBrakeLock01 * 0.44f, 0.72f, 1.42f) :
			std::clamp(1.0f + trailBrake01 * 0.62f * (1.0f - hardBrakeLock01 * 0.72f) + liftRotation01 * 0.58f - highSpeedStability01 * 0.02f, 0.88f, 1.72f);
		const float lateralRequest01 =
			std::abs(rawLateralForce) * lateralCurveRequestScale /
			std::max(powerLimitedLateralGrip, 1.0f);
		const float lateralSlideScale =
			bFront ?
			std::clamp(0.86f + trailBrake01 * 0.08f - hardBrakeLock01 * 0.22f, 0.62f, 0.94f) :
			std::clamp(0.90f - liftRotation01 * 0.22f - trailBrake01 * 0.24f + hardBrakeLock01 * 0.08f + highSpeedStability01 * 0.02f, 0.58f, 0.94f);
		float lateralForce =
			VehicleSignNonZero(rawLateralForce) *
			powerLimitedLateralGrip *
			tireFrictionCurve(lateralRequest01, lateralSlideScale, bFront ? 1.65f : 1.75f);
		const float lateralUsage01 =
			std::clamp(std::abs(lateralForce) / std::max(powerLimitedLateralGrip, 1.0f), 0.0f, 1.0f);
		const float remainingLongGrip =
			longitudinalGrip *
			std::sqrt(std::max(0.035f, 1.0f - lateralUsage01 * lateralUsage01 * 0.92f));
		const float longRequest01 = std::abs(desiredLongForce) / std::max(remainingLongGrip, 1.0f);
		float longForce =
			VehicleSignNonZero(desiredLongForce) *
			remainingLongGrip *
			tireFrictionCurve(longRequest01, 0.86f, 1.55f);
		const glm::vec3 tireForce = wheelFwd * longForce + wheelRight * lateralForce;

		totalForce += tireForce;
		totalYawTorque += r.z * tireForce.x - r.x * tireForce.z;
		const float drivetrainForceError = driveLongForce + engineBrakeLongForce - longForce;
		const float driveSpinOmega =
			std::clamp(drivetrainForceError / std::max(grip, 1.0f), -1.8f, 1.8f) *
			(42.0f + std::abs(VehicleDriving.Throttle) * 48.0f);
		const float brakeSpinOmega =
			-VehicleSignNonZero(std::abs(wheel.AngularVelocity) > 0.1f ? wheel.AngularVelocity : vLong) *
			std::clamp(brakeDemandForce / std::max(grip, 1.0f), 0.0f, 1.0f) *
			42.0f;
		const float targetWheelAngularVelocity = vLong / kWheelPhysicsRadius + driveSpinOmega + brakeSpinOmega;
		const float wheelOmegaBlend = std::clamp(dt * (11.0f + std::abs(longForce) / std::max(grip, 1.0f) * 8.0f), 0.0f, 1.0f);
		wheel.AngularVelocity += (targetWheelAngularVelocity - wheel.AngularVelocity) * wheelOmegaBlend;
		wheel.AngularVelocity *= std::pow(0.985f, dt);
		wheel.SpinAngle += wheel.AngularVelocity * dt;
		const float lateralUtilization01 =
			std::clamp(std::abs(lateralForce) / std::max(powerLimitedLateralGrip * 0.92f, 1.0f), 0.0f, 1.0f);
		const float longitudinalUtilization01 =
			std::clamp(std::abs(longForce) / std::max(longitudinalGrip, 1.0f), 0.0f, 1.0f);
		wheel.SlipRatio = std::clamp((desiredLongForce - longForce) / std::max(longitudinalGrip, 1.0f) + rearPowerSlip01 * 0.55f, -1.0f, 1.0f);
		wheel.SlipAngle = std::clamp(tireSlipAngle / glm::radians(18.0f) * (0.45f + lateralUtilization01 * 0.55f + longitudinalUtilization01 * 0.18f), -1.0f, 1.0f);
	}

	VehicleDriving.GroundedFraction = static_cast<float>(groundedWheels) * 0.25f;
	if (groundedWheels > 0)
	{
		totalForce += -VehicleDriving.Velocity * (kMass * 0.06f);
		const float speedSq = glm::dot(VehicleDriving.Velocity, VehicleDriving.Velocity);
		if (speedSq > 1.0f)
			totalForce += -glm::normalize(VehicleDriving.Velocity) * (0.010f * speedSq);
	}

	glm::vec3 acceleration = totalForce / kMass;
	acceleration.y = 0.0f;
	VehicleDriving.Velocity += acceleration * dt;
	const float rotationIntent01 =
		std::clamp(liftRotationDemand01 * 0.92f + lightBrakeTurnIn01 * 0.62f, 0.0f, 1.0f);
	const float yawDampingRate =
		std::max(
			0.82f,
			(1.35f +
				highSpeedStability01 * (3.20f + preYawRate01 * 2.10f) +
				brakingStability01 * 1.10f) *
			(1.0f - rotationIntent01 * 0.42f));
	totalYawTorque -= VehicleDriving.YawRate * kYawInertia * yawDampingRate;
	VehicleDriving.YawRate += (totalYawTorque / kYawInertia) * dt;
	const float highSpeedYawDamping =
		std::clamp(highSpeedStability01 * (0.45f + preYawRate01 * 0.55f) * (1.0f - rotationIntent01 * 0.56f), 0.0f, 1.0f);
	VehicleDriving.YawRate *= std::pow(glm::mix(0.82f, 0.48f, highSpeedYawDamping), dt);
	VehicleDriving.Yaw += VehicleDriving.YawRate * dt;

	glm::vec3 desiredPosition = VehicleDriving.Position + VehicleDriving.Velocity * dt;
	if (groundedWheels > 0)
	{
		const float targetY = groundHeightSum / static_cast<float>(groundedWheels) + kChassisRideHeight;
		VehicleDriving.VerticalVelocity += (targetY - VehicleDriving.Position.y) * 18.0f * dt;
		VehicleDriving.VerticalVelocity *= std::pow(0.025f, dt);
		desiredPosition.y = VehicleDriving.Position.y + VehicleDriving.VerticalVelocity * dt;
	}
	else
	{
		VehicleDriving.VerticalVelocity -= kGravity * dt;
		desiredPosition.y = VehicleDriving.Position.y + VehicleDriving.VerticalVelocity * dt;
	}

	const glm::vec3 horizontalMove(desiredPosition.x - oldPosition.x, 0.0f, desiredPosition.z - oldPosition.z);
	const float horizontalDistance = glm::length(horizontalMove);
	if (horizontalDistance > 0.01f)
	{
		CpuPhysicsRaycastHit sweepHit;
		if (CpuPhysicsSphereSweep(oldPosition, kCollisionRadius, horizontalMove / horizontalDistance, horizontalDistance, sweepHit) &&
			sweepHit.Normal.y < 0.65f)
		{
			const float safeDistance = std::min(horizontalDistance, std::max(0.0f, sweepHit.Distance - kCollisionSkin));
			glm::vec3 resolved = oldPosition + horizontalMove / horizontalDistance * safeDistance;
			glm::vec3 normal = glm::normalize(glm::vec3(sweepHit.Normal.x, 0.0f, sweepHit.Normal.z));
			if (glm::length(normal) > 0.001f)
			{
				const float intoWall = glm::dot(VehicleDriving.Velocity, normal);
				if (intoWall < 0.0f)
					VehicleDriving.Velocity -= normal * intoWall * 1.15f;
			}
			resolved.y = desiredPosition.y;
			VehicleDriving.Position = resolved;
		}
		else
		{
			VehicleDriving.Position = desiredPosition;
		}
	}
	else
	{
		VehicleDriving.Position = desiredPosition;
	}

	const glm::vec3 velocityDelta = VehicleDriving.Velocity - oldVelocity;
	VehicleDriving.LongitudinalAccel = glm::dot(velocityDelta / dt, forward);
	VehicleDriving.LateralAccel = glm::dot(velocityDelta / dt, right);
	VehicleDriving.VisualPitch += (std::clamp(-VehicleDriving.LongitudinalAccel / 4300.0f, -0.026f, 0.026f) - VehicleDriving.VisualPitch) *
		std::clamp(dt * 3.8f, 0.0f, 1.0f);
	VehicleDriving.VisualRoll += (std::clamp(VehicleDriving.LateralAccel / 4300.0f, -0.030f, 0.030f) - VehicleDriving.VisualRoll) *
		std::clamp(dt * 3.8f, 0.0f, 1.0f);
	VehicleDriving.WheelSpin += speedForward * dt / kWheelPhysicsRadius;

	UpdateVehicleVisuals();

	const glm::vec3 newForward = VehicleForwardFromYaw(VehicleDriving.Yaw);
	const glm::vec3 cameraTarget = VehicleDriving.Position + glm::vec3(0.0f, 28.0f, 0.0f) + newForward * 520.0f;
	const glm::vec3 desiredCamera = VehicleDriving.Position - newForward * 115.0f + glm::vec3(0.0f, 44.0f, 0.0f);
	const float cameraBlend = std::clamp(dt * 7.0f, 0.0f, 1.0f);
	if (glm::length(m_camera.m_position) < 0.001f)
		m_camera.m_position = desiredCamera;
	else
		m_camera.m_position = glm::mix(m_camera.m_position, desiredCamera, cameraBlend);

	glm::vec3 look = glm::normalize(cameraTarget - m_camera.m_position);
	m_camera.m_lookDirection = look;
	m_camera.m_upDirection = glm::vec3(0.0f, 1.0f, 0.0f);
	m_camera.m_yaw = std::atan2(look.x, look.z);
	m_camera.m_pitch = std::asin(std::clamp(look.y, -0.99f, 0.99f));
	m_camera.m_keysPressed = {};
	bScriptCameraControlEnabled = false;
	UpdateMainCameraEntityFromSimpleCamera();
}

void Corona::UpdateVehicleVisuals()
{
	if (!VehicleDriving.bSpawned)
		return;

	const float yaw = VehicleDriving.Yaw;
	const float pitch = VehicleDriving.VisualPitch;
	const float roll = VehicleDriving.VisualRoll;
	const glm::vec3 bodyCenter = VehicleDriving.Position + RotateVehicleLocal(yaw, pitch, roll, glm::vec3(0.0f, 8.0f * kVehicleVisualScale, 0.0f));
	const glm::vec3 cabinCenter = VehicleDriving.Position + RotateVehicleLocal(yaw, pitch, roll, glm::vec3(0.0f, 46.0f * kVehicleVisualScale, -34.0f * kVehicleVisualScale));
	SetSceneObjectTransform(VehicleDriving.BodyHandle, BuildVehiclePartTransform(VehicleBodyScene, glm::vec3(176.0f, 42.0f, 420.0f) * kVehicleVisualScale, bodyCenter, yaw, pitch, roll));
	SetSceneObjectTransform(VehicleDriving.CabinHandle, BuildVehiclePartTransform(VehicleCabinScene, glm::vec3(126.0f, 46.0f, 142.0f) * kVehicleVisualScale, cabinCenter, yaw, pitch, roll));

	constexpr float kWheelBase = 282.0f * kVehicleVisualScale;
	constexpr float kTrack = 164.0f * kVehicleVisualScale;
	constexpr float kWheelVisualRayStart = 70.0f * kVehicleVisualScale;
	constexpr float kWheelVisualRayLength = 175.0f * kVehicleVisualScale;
	constexpr float kWheelVisualRadius = 29.0f * kVehicleVisualScale;
	auto smooth01 = [](float edge0, float edge1, float value) -> float
	{
		const float t = std::clamp((value - edge0) / std::max(edge1 - edge0, 0.0001f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	const float speed = glm::length(glm::vec2(VehicleDriving.Velocity.x, VehicleDriving.Velocity.z));
	const float highSpeedStability01 = smooth01(6200.0f, 9800.0f, speed);
	const float highGear01 = smooth01(2.5f, 4.0f, static_cast<float>(VehicleDriving.Gear));
	const float highGearLiftOffSteerRelief01 =
		std::clamp(
			VehicleDriving.LiftOffOversteer *
			highGear01 *
			smooth01(3000.0f, 8200.0f, speed),
			0.0f,
			1.0f);
	const float highSpeedSteerPenalty =
		highSpeedStability01 * std::max(0.10f, 0.35f - highGearLiftOffSteerRelief01 * 0.26f);
	const float maxSteerAngle =
		glm::radians(34.0f) / (1.0f + speed * 0.00014f + highSpeedSteerPenalty);
	const float brakingStability01 =
		std::clamp(VehicleDriving.Brake * smooth01(2200.0f, 7000.0f, speed) * (0.35f + std::abs(VehicleDriving.Steering) * 0.65f), 0.0f, 1.0f);
	const float visualAlignmentScale = VehicleAlignment.VisualAlignmentScale;
	const std::array<glm::vec3, 4> wheelLocal = {
		glm::vec3(-kTrack * 0.5f, -36.0f * kVehicleVisualScale,  kWheelBase * 0.5f),
		glm::vec3( kTrack * 0.5f, -36.0f * kVehicleVisualScale,  kWheelBase * 0.5f),
		glm::vec3(-kTrack * 0.5f, -36.0f * kVehicleVisualScale, -kWheelBase * 0.5f),
		glm::vec3( kTrack * 0.5f, -36.0f * kVehicleVisualScale, -kWheelBase * 0.5f)
	};
	const std::array<const char*, 4> wheelLabels = { "FR", "FL", "RR", "RL" };

	for (int i = 0; i < 4; ++i)
	{
		const bool bFront = i == kVehicleWheelFL || i == kVehicleWheelFR;
		const bool bRight = i == kVehicleWheelFR || i == kVehicleWheelRR;
		const float sideSign = bRight ? 1.0f : -1.0f;
		const VehicleWheelState& wheel = VehicleDriving.Wheels[static_cast<size_t>(i)];
		const float baseSteerAngle = bFront ? VehicleDriving.Steering * maxSteerAngle : 0.0f;
		const float staticToeAngle =
			bFront ?
			sideSign * glm::radians(VehicleAlignment.FrontStaticToeDeg * visualAlignmentScale) :
			-sideSign * glm::radians(VehicleAlignment.RearStaticToeDeg * visualAlignmentScale);
		const float rollToeAngle =
			(bFront ? VehicleAlignment.FrontRollToeGain : VehicleAlignment.RearRollToeGain) *
			visualAlignmentScale *
			VehicleDriving.VisualRoll *
			sideSign;
		const float brakeToeStabilizeAngle =
			bFront ?
			-baseSteerAngle * brakingStability01 * VehicleAlignment.FrontBrakeToeStabilize * visualAlignmentScale :
			-VehicleDriving.YawRate * brakingStability01 * VehicleAlignment.RearBrakeToeStabilize * visualAlignmentScale;
		const float steerAngle = baseSteerAngle + staticToeAngle + rollToeAngle + brakeToeStabilizeAngle;
		const float visualRoll01 = std::clamp(std::abs(VehicleDriving.VisualRoll) / 0.050f, 0.0f, 1.0f);
		const float visualLoad01 = std::clamp((wheel.Load / std::max(1.0f, 0.25f * 1450.0f * 980.0f) - 1.0f) / 0.75f, 0.0f, 1.0f);
		const float visualNegativeCamber =
			(glm::radians(bFront ? VehicleAlignment.FrontStaticNegativeCamberDeg : VehicleAlignment.RearStaticNegativeCamberDeg) +
			glm::radians(bFront ? VehicleAlignment.FrontRollDynamicCamberDeg : VehicleAlignment.RearRollDynamicCamberDeg) *
				visualRoll01 *
				(0.35f + visualLoad01 * 0.65f)) *
			visualAlignmentScale;
		const float visualCamberAngle = sideSign * visualNegativeCamber;
		glm::vec3 wheelCenter = VehicleDriving.Position + RotateVehicleLocal(yaw, 0.0f, 0.0f, wheelLocal[i]);
		if (wheel.bGrounded)
			wheelCenter.y = wheel.ContactPoint.y + kWheelVisualRadius;
		SetSceneObjectTransform(
			VehicleDriving.WheelHandles[static_cast<size_t>(i)],
			BuildVehicleWheelTransform(
				VehicleWheelScene,
				glm::vec3(30.0f, 58.0f, 58.0f) * kVehicleVisualScale,
				wheelCenter,
				yaw + steerAngle,
				visualCamberAngle,
				wheel.SpinAngle));

		if (VehicleDriving.LoadBarHandles[static_cast<size_t>(i)] != InvalidSceneObjectHandle)
		{
			const float normalizedLoad =
				std::clamp(
					VehicleDriving.Wheels[static_cast<size_t>(i)].Load / (1350.0f * 980.0f * 0.34f),
					0.02f,
					1.35f);
			const float barHeight = (8.0f + normalizedLoad * 96.0f) * kVehicleVisualScale;
			const glm::vec3 barBase =
				VehicleDriving.Position +
				RotateVehicleLocal(yaw, 0.0f, 0.0f, glm::vec3(wheelLocal[i].x, 0.0f, wheelLocal[i].z)) +
				glm::vec3(0.0f, 104.0f * kVehicleVisualScale, 0.0f);
			const glm::vec3 barCenter = barBase + glm::vec3(0.0f, barHeight * 0.5f, 0.0f);
			SetSceneObjectTransform(
				VehicleDriving.LoadBarHandles[static_cast<size_t>(i)],
				BuildVehiclePartTransform(
					VehicleLoadBarScene,
					glm::vec3(10.0f * kVehicleVisualScale, barHeight, 10.0f * kVehicleVisualScale),
					barCenter,
					yaw,
					0.0f,
					0.0f));

			const glm::vec4 labelColor =
				bFront ?
				glm::vec4(0.35f, 0.92f, 1.0f, 1.0f) :
				glm::vec4(1.0f, 0.86f, 0.28f, 1.0f);
			const glm::vec3 labelPosition =
				barBase +
				glm::vec3(0.0f, barHeight + 18.0f * kVehicleVisualScale, 0.0f);
			QueueScriptUiWorldTextForScript(
				std::string("vehicle_load_label_") + wheelLabels[static_cast<size_t>(i)],
				labelPosition,
				wheelLabels[static_cast<size_t>(i)],
				0.0f,
				0.0f,
				labelColor);

			if (VehicleDriving.SlipPlaneHandles[static_cast<size_t>(i)] != InvalidSceneObjectHandle)
			{
				const float slipRatio01 = std::abs(wheel.SlipRatio);
				const float slipAngle01 = std::abs(wheel.SlipAngle);
				const float slip01 =
					std::clamp(
						std::sqrt(slipRatio01 * slipRatio01 + slipAngle01 * slipAngle01 * 1.65f),
						0.0f,
						1.25f);
				const float slipHeight = (5.0f + slip01 * 76.0f) * kVehicleVisualScale;
				const float slipWidth = (18.0f + slip01 * 138.0f) * kVehicleVisualScale;
				const glm::vec3 slipOffset =
					RotateVehicleLocal(
						yaw,
						0.0f,
						0.0f,
						glm::vec3(sideSign * (24.0f * kVehicleVisualScale + slipWidth * 0.5f), 0.0f, 0.0f));
				const glm::vec3 slipCenter =
					barBase +
					slipOffset +
					glm::vec3(0.0f, slipHeight * 0.5f, 0.0f);
				SetSceneObjectTransform(
					VehicleDriving.SlipPlaneHandles[static_cast<size_t>(i)],
					BuildVehiclePartTransform(
						VehicleSlipPlaneScene,
						glm::vec3(slipWidth, slipHeight, 2.5f * kVehicleVisualScale),
						slipCenter,
						yaw,
						0.0f,
						0.0f));
			}
		}
	}
}

void Corona::DrawVehicleDrivingOverlay()
{
	if (!bVehicleDrivingMode || !VehicleDriving.bSpawned)
		return;

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing;
	ImGui::SetNextWindowPos(ImVec2(18.0f, 18.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.58f);
	if (ImGui::Begin("Vehicle Driving HUD", nullptr, flags))
	{
		const float speedKmh = glm::length(glm::vec2(VehicleDriving.Velocity.x, VehicleDriving.Velocity.z)) * 0.036f;
		const std::string gearLabel = VehicleDriving.Gear == 0 ? "R" : std::to_string(VehicleDriving.Gear);
		const float rpm01 = std::clamp((VehicleDriving.EngineRpm - 900.0f) / (7200.0f - 900.0f), 0.0f, 1.0f);
		ImGui::SetWindowFontScale(2.6f);
		if (VehicleDriving.EngineRpm > 6800.0f)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.20f, 0.12f, 1.0f));
		ImGui::Text("GEAR %s", gearLabel.c_str());
		ImGui::SameLine(170.0f);
		ImGui::Text("%.0f RPM", VehicleDriving.EngineRpm);
		if (VehicleDriving.EngineRpm > 6800.0f)
			ImGui::PopStyleColor();
		ImGui::SetWindowFontScale(1.0f);
		ImGui::ProgressBar(rpm01, ImVec2(330.0f, 14.0f), "");
		ImGui::Separator();
		ImGui::Text("Vehicle mode");
		ImGui::Text("W/S throttle  A/D steer  Space brake  R reset");
		ImGui::Text("Gamepad: RT throttle  LT brake  X downshift/R  A upshift");
		if (ImGui::Button(bVehicleSettingsViewerOpen ? "Hide vehicle settings" : "Show vehicle settings"))
			bVehicleSettingsViewerOpen = !bVehicleSettingsViewerOpen;
		ImGui::Separator();
		if (VehicleDriving.Gear == 0)
			ImGui::Text("Speed %.1f km/h  Gear R", speedKmh);
		else
			ImGui::Text("Speed %.1f km/h  Gear %d", speedKmh, VehicleDriving.Gear);
		const float throttleGauge01 = std::clamp(std::abs(VehicleDriving.Throttle), 0.0f, 1.0f);
		const float brakeGauge01 = std::clamp(VehicleDriving.Brake, 0.0f, 1.0f);
		ImGui::Text("Accel");
		ImGui::SameLine(66.0f);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.10f, 0.74f, 0.22f, 1.0f));
		ImGui::ProgressBar(throttleGauge01, ImVec2(220.0f, 18.0f), "");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::Text("%.0f%%", throttleGauge01 * 100.0f);
		ImGui::Text("Brake");
		ImGui::SameLine(66.0f);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.92f, 0.18f, 0.10f, 1.0f));
		ImGui::ProgressBar(brakeGauge01, ImVec2(220.0f, 18.0f), "");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::Text("%.0f%%", brakeGauge01 * 100.0f);
		ImGui::Text("Throttle %.2f  Brake %.2f  Steer %.2f", VehicleDriving.Throttle, VehicleDriving.Brake, VehicleDriving.Steering);
		ImGui::Text("Lift-off %.2f  Oversteer %.2f", VehicleDriving.LiftOffOversteer, VehicleDriving.OversteerSustain);
		const float frontSharePct =
			(VehicleDriving.Wheels[kVehicleWheelFL].DriveTorqueShare + VehicleDriving.Wheels[kVehicleWheelFR].DriveTorqueShare) * 100.0f;
		const float rearSharePct =
			(VehicleDriving.Wheels[kVehicleWheelRL].DriveTorqueShare + VehicleDriving.Wheels[kVehicleWheelRR].DriveTorqueShare) * 100.0f;
		ImGui::Text("Diff F/R %.0f/%.0f  FL %.0f FR %.0f RL %.0f RR %.0f%%",
			frontSharePct,
			rearSharePct,
			VehicleDriving.Wheels[kVehicleWheelFL].DriveTorqueShare * 100.0f,
			VehicleDriving.Wheels[kVehicleWheelFR].DriveTorqueShare * 100.0f,
			VehicleDriving.Wheels[kVehicleWheelRL].DriveTorqueShare * 100.0f,
			VehicleDriving.Wheels[kVehicleWheelRR].DriveTorqueShare * 100.0f);
		ImGui::Text("Accel L %.0f  Lat %.0f cm/s^2", VehicleDriving.LongitudinalAccel, VehicleDriving.LateralAccel);
		ImGui::Text("Ground %.0f%%", VehicleDriving.GroundedFraction * 100.0f);
		const char* labels[4] = { "FR", "FL", "RR", "RL" };
		for (int i = 0; i < 4; ++i)
		{
			const VehicleWheelState& wheel = VehicleDriving.Wheels[static_cast<size_t>(i)];
			ImGui::Text("%s", labels[i]);
			ImGui::SameLine(34.0f);
			const float load01 = std::clamp(wheel.Load / (1350.0f * 980.0f * 0.36f), 0.0f, 1.0f);
			ImGui::ProgressBar(load01, ImVec2(132.0f, 0.0f), wheel.bGrounded ? "" : "air");
		}
	}
	ImGui::End();
}

void Corona::DrawVehicleSettingsViewer()
{
	if (!bVehicleDrivingMode || !VehicleDriving.bSpawned || !bVehicleSettingsViewerOpen)
		return;

	ImGui::SetNextWindowPos(ImVec2(18.0f, 330.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(430.0f, 500.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Vehicle Settings", &bVehicleSettingsViewerOpen, ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::End();
		return;
	}

	bool bChanged = false;
	auto dragDegrees = [&bChanged](const char* label, float* value, float minValue, float maxValue)
	{
		bChanged |= ImGui::DragFloat(label, value, 0.01f, minValue, maxValue, "%.2f deg");
	};
	auto dragScalar = [&bChanged](const char* label, float* value, float speed, float minValue, float maxValue)
	{
		bChanged |= ImGui::DragFloat(label, value, speed, minValue, maxValue, "%.4f");
	};

	ImGui::TextUnformatted("Alignment");
	ImGui::Separator();
	dragDegrees("Front static toe", &VehicleAlignment.FrontStaticToeDeg, -5.0f, 5.0f);
	dragDegrees("Rear static toe", &VehicleAlignment.RearStaticToeDeg, -5.0f, 5.0f);
	dragDegrees("Front static -camber", &VehicleAlignment.FrontStaticNegativeCamberDeg, 0.0f, 10.0f);
	dragDegrees("Rear static -camber", &VehicleAlignment.RearStaticNegativeCamberDeg, 0.0f, 10.0f);
	dragDegrees("Front roll camber gain", &VehicleAlignment.FrontRollDynamicCamberDeg, 0.0f, 12.0f);
	dragDegrees("Rear roll camber gain", &VehicleAlignment.RearRollDynamicCamberDeg, 0.0f, 12.0f);
	dragScalar("Front roll toe gain", &VehicleAlignment.FrontRollToeGain, 0.001f, -1.0f, 1.0f);
	dragScalar("Rear roll toe gain", &VehicleAlignment.RearRollToeGain, 0.001f, -1.0f, 1.0f);
	dragScalar("Front brake toe stabilize", &VehicleAlignment.FrontBrakeToeStabilize, 0.005f, 0.0f, 2.0f);
	dragScalar("Rear brake toe stabilize", &VehicleAlignment.RearBrakeToeStabilize, 0.001f, 0.0f, 0.20f);
	dragScalar("Visual alignment scale", &VehicleAlignment.VisualAlignmentScale, 0.01f, 0.0f, 5.0f);

	if (bChanged)
	{
		ClampVehicleAlignmentSettings();
		UpdateVehicleVisuals();
	}

	ImGui::Separator();
	if (ImGui::Button("Save"))
		SaveVehicleAlignmentSettings();
	ImGui::SameLine();
	if (ImGui::Button("Reload"))
	{
		LoadVehicleAlignmentSettings();
		UpdateVehicleVisuals();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset defaults"))
	{
		VehicleAlignment = VehicleAlignmentSettings{};
		ClampVehicleAlignmentSettings();
		UpdateVehicleVisuals();
		VehicleAlignmentStatus = L"Reset alignment to defaults; press Save to persist.";
	}

	const std::string pathUtf8 = PlatformWideToUtf8(GetVehicleAlignmentSettingsPath());
	ImGui::TextWrapped("JSON: %s", pathUtf8.c_str());
	if (!VehicleAlignmentStatus.empty())
	{
		const std::string statusUtf8 = PlatformWideToUtf8(VehicleAlignmentStatus);
		ImGui::TextWrapped("%s", statusUtf8.c_str());
	}

	ImGui::Separator();
	ImGui::Text("Runtime");
	ImGui::Text("Steer %.2f  Lift-off %.2f  Oversteer %.2f",
		VehicleDriving.Steering,
		VehicleDriving.LiftOffOversteer,
		VehicleDriving.OversteerSustain);
	ImGui::Text("Pitch %.3f  Roll %.3f  YawRate %.3f",
		VehicleDriving.VisualPitch,
		VehicleDriving.VisualRoll,
		VehicleDriving.YawRate);

	ImGui::End();
}
