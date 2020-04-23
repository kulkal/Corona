//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "glm/glm.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/transform.hpp"
#include "glm/mat4x4.hpp"
#include "glm/fwd.hpp"
#include "glm/gtc/constants.hpp"


class SimpleCamera
{
public:
	SimpleCamera();

	void Init(glm::vec3 position);
	void Update(float elapsedSeconds);
	glm::mat4x4 GetViewMatrix();
	glm::mat4x4 GetProjectionMatrix(float fov, float aspectRatio, float nearPlane = 1.0f, float farPlane = 10000.0f);
	void SetMoveSpeed(float unitsPerSecond);
	void SetTurnSpeed(float radiansPerSecond);

	void OnKeyDown(WPARAM key);
	void OnKeyUp(WPARAM key);


public:
	void Reset();

	struct KeysPressed
	{
		bool w;
		bool a;
		bool s;
		bool d;

		bool q;
		bool e;

		bool left;
		bool right;
		bool up;
		bool down;
	};

	glm::vec3 m_initialPosition;
	glm::vec3 m_position;
	float m_yaw;				// Relative to the +z axis.
	float m_pitch;				// Relative to the xz plane.
	glm::vec3 m_lookDirection;
	glm::vec3 m_upDirection;
	float m_moveSpeed;			// Speed at which the camera moves, in units per second.
	float m_turnSpeed;			// Speed at which the camera turns, in radians per second.

	KeysPressed m_keysPressed;
};
