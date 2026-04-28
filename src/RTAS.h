#pragma once

class Mesh;

class RTAS
{
public:
	Mesh* MeshPtr = nullptr;
	virtual ~RTAS() = default;
};
