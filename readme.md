# DX12 & DXR test framework
This project is for dx12 & dxr learning. Quite  unorgnized for now. I am trying to test best practice implementation(resource binding, dynamic descriotpr allocation, multi-thread command submission, etc....)

## DX12
* Helper classes for resource management.
* Easy resource binding interface.
* Dynamic descriptor allocation strategy.
* Multi threaded mesh rendering.

## DXR
* Shadow
* Reflection

## Build
* I used to use CMake before, considering to migrate to other build system.
* Currently, it is just a vs solution.

## Third-party libs
* [enkiTS](https://github.com/dougbinks/enkiTS)
* [glm](https://glm.g-truc.net/0.9.9/index.html)

## Usefule link
* [Dynamic cb implementation](https://github.com/Microsoft/DirectX-Graphics-Samples/blob/07008938a0dc5a187a23abcb55b61f8c2809c874/Samples/Desktop/D3D12PipelineStateCache/src/DynamicConstantBuffer.cpp#L64)
* [MJP github](https://github.com/TheRealMJP)
	* Excellent dx12 reference implementation.