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
I used to cmake before, now using premake. it is much simpler to use and have most of features I needed. I tried scons too but it is a little bit difficult than premake but feels more professional. I felt that premake is better for personal rendering test frameworkk.
* Go to vs directory.
* premake5.exe vs2017

## Third-party libs
* [enkiTS](https://github.com/dougbinks/enkiTS)
* [glm](https://glm.g-truc.net/0.9.9/index.html)
* [NV Aftermath](https://developer.nvidia.com/nvidia-aftermath)

## Usefule link
* [Dynamic cb implementation](https://github.com/Microsoft/DirectX-Graphics-Samples/blob/07008938a0dc5a187a23abcb55b61f8c2809c874/Samples/Desktop/D3D12PipelineStateCache/src/DynamicConstantBuffer.cpp#L64)
* [MJP github](https://github.com/TheRealMJP)
	* Excellent dx12 reference implementation.