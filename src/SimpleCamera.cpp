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

#include "stdafx.h"
#include "SimpleCamera.h"

namespace
{
	constexpr uint32_t kKeyLeft = 0x25;
	constexpr uint32_t kKeyUp = 0x26;
	constexpr uint32_t kKeyRight = 0x27;
	constexpr uint32_t kKeyDown = 0x28;
	constexpr uint32_t kKeyEscape = 0x1B;
}

SimpleCamera::SimpleCamera():
	m_initialPosition(0, 0, 0),
	m_position(m_initialPosition),
	m_yaw(0),
	m_pitch(0),
	m_lookDirection(0, 0, -1),
	m_upDirection(0, 1, 0),
	m_moveSpeed(200.0f),
	m_turnSpeed(glm::half_pi<float>()),
	m_virtualMoveInput(0.0f),
	m_keysPressed{},
	m_mouseButtonDown(false),
	m_lastMouseX(0),
	m_lastMouseY(0),
	m_mouseSensitivity(0.003f)
{
}

void SimpleCamera::Init(glm::vec3 position)
{
	m_initialPosition = position;
	Reset();
}

void SimpleCamera::SetMoveSpeed(float unitsPerSecond)
{
	m_moveSpeed = unitsPerSecond;
}

void SimpleCamera::SetTurnSpeed(float radiansPerSecond)
{
	m_turnSpeed = radiansPerSecond;
}

void SimpleCamera::SetVirtualMoveInput(float strafe, float forward, float vertical)
{
	m_virtualMoveInput = glm::vec3(strafe, vertical, forward);
}

void SimpleCamera::AddLookDelta(float deltaX, float deltaY)
{
	m_yaw -= deltaX * m_mouseSensitivity;
	m_pitch -= deltaY * m_mouseSensitivity;

	m_pitch = glm::min(m_pitch, glm::quarter_pi<float>());
	m_pitch = glm::max(-glm::quarter_pi<float>(), m_pitch);
}

void SimpleCamera::Reset()
{
	m_position = m_initialPosition;
	m_yaw = 4.4;// glm::pi<float>();
	m_pitch = -0.40f;
	m_lookDirection = { 0, 0, -1 };
}

void SimpleCamera::Update(float elapsedSeconds)
{
	// Calculate the move vector in camera space.
	glm::vec3 move(0, 0, 0);

	if (m_keysPressed.a)
		move.x -= 1.0f;
	if (m_keysPressed.d)
		move.x += 1.0f;
	if (m_keysPressed.w)
		move.z -= 1.0f;
	if (m_keysPressed.s)
		move.z += 1.0f;

	if (m_keysPressed.q)
		move.y -= 1.0f;
	if (m_keysPressed.e)
		move.y += 1.0f;

	move += m_virtualMoveInput;
	const float moveLength = glm::length(move);
	if (moveLength > 1.0f)
		move /= moveLength;

	/*if (fabs(move.x) > 0.1f && fabs(move.z) > 0.1f)
	{
		XMVECTOR vector = XMVector3Normalize(XMLoadFloat3(&move));
		move.x = XMVectorGetX(vector);
		move.z = XMVectorGetZ(vector);
	}*/

	float moveInterval = m_moveSpeed * elapsedSeconds;
	float rotateInterval = m_turnSpeed * elapsedSeconds;

	if (m_keysPressed.left)
		m_yaw += rotateInterval;
	if (m_keysPressed.right)
		m_yaw -= rotateInterval;
	if (m_keysPressed.up)
		m_pitch += rotateInterval;
	if (m_keysPressed.down)
		m_pitch -= rotateInterval;

	// Prevent looking too far up or down.
	m_pitch = glm::min(m_pitch, glm::quarter_pi<float>());
	m_pitch = glm::max(-glm::quarter_pi<float>(), m_pitch);

	// Move the camera in model space.
	float x = move.x * -cosf(m_yaw) - move.z * sinf(m_yaw);
	float z = move.x * sinf(m_yaw) - move.z * cosf(m_yaw);
	m_position.x += x * moveInterval;
	m_position.z += z * moveInterval;

	m_position.y += move.y * moveInterval;


	// Determine the look direction.
	float r = cosf(m_pitch);
	m_lookDirection.x = r * sinf(m_yaw);
	m_lookDirection.y = sinf(m_pitch);
	m_lookDirection.z = r * cosf(m_yaw);
}

glm::mat4x4 SimpleCamera::GetViewMatrix()
{
	return glm::lookAtRH(m_position, m_position + m_lookDirection, m_upDirection);
}

glm::mat4x4 SimpleCamera::GetProjectionMatrix(float fov, float aspectRatio, float nearPlane, float farPlane)
{
	return glm::perspectiveRH<float>(fov, aspectRatio, nearPlane, farPlane);
}

void SimpleCamera::OnKeyDown(uint32_t key)
{
	switch (key)
	{
	case 'W':
		m_keysPressed.w = true;
		break;
	case 'A':
		m_keysPressed.a = true;
		break;
	case 'S':
		m_keysPressed.s = true;
		break;
	case 'D':
		m_keysPressed.d = true;
		break;
	case 'Q':
		m_keysPressed.q = true;
		break;
	case 'E':
		m_keysPressed.e = true;
		break;
	case kKeyLeft:
		m_keysPressed.left = true;
		break;
	case kKeyRight:
		m_keysPressed.right = true;
		break;
	case kKeyUp:
		m_keysPressed.up = true;
		break;
	case kKeyDown:
		m_keysPressed.down = true;
		break;
	case kKeyEscape:
		Reset();
		break;
	}
}

void SimpleCamera::OnKeyUp(uint32_t key)
{
	switch (key)
	{
	case 'W':
		m_keysPressed.w = false;
		break;
	case 'A':
		m_keysPressed.a = false;
		break;
	case 'S':
		m_keysPressed.s = false;
		break;
	case 'D':
		m_keysPressed.d = false;
		break;
	case 'Q':
		m_keysPressed.q = false;
		break;
	case 'E':
		m_keysPressed.e = false;
		break;
	case kKeyLeft:
		m_keysPressed.left = false;
		break;
	case kKeyRight:
		m_keysPressed.right = false;
		break;
	case kKeyUp:
		m_keysPressed.up = false;
		break;
	case kKeyDown:
		m_keysPressed.down = false;
		break;
	}
}

void SimpleCamera::OnMouseDown(int x, int y)
{
	m_mouseButtonDown = true;
	m_lastMouseX = x;
	m_lastMouseY = y;
}

void SimpleCamera::OnMouseUp()
{
	m_mouseButtonDown = false;
}

void SimpleCamera::OnMouseMove(int x, int y)
{
	if (m_mouseButtonDown)
	{
		int deltaX = x - m_lastMouseX;
		int deltaY = y - m_lastMouseY;

		AddLookDelta(static_cast<float>(deltaX), static_cast<float>(deltaY));

		m_lastMouseX = x;
		m_lastMouseY = y;
	}
}
