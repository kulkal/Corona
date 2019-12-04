# DX12 & DXR test framework
This project is for dx12 & dxr learning. I am trying to test best practice implementation(resource binding, descriotpr allocation, multi-thread command submission, etc....)

## DX12 Topic
* Easy resource binding interface.
* Descriptor allocation.
	* Ring buffer.
* Multi threaded mesh rendering. (this is broken temporarily)
* Descriptor table usage.
* Memory residency.

## DXR Topic
* How to make resource binding easier.
	* Root signature.
	* Shader table.

## Build
* Go to build directory.
* premake5.exe vs2017

## Third-party libs
* [enkiTS](https://github.com/dougbinks/enkiTS)
* [glm](https://glm.g-truc.net/0.9.9/index.html)
* [NV Aftermath](https://developer.nvidia.com/nvidia-aftermath)
* [premake](https://premake.github.io/)

## Useful link
* [MJP github](https://github.com/TheRealMJP)
	* Excellent dx12 reference implementation.

## Fun bug story
* Shader table should be ring buffer if it changes!.
	
## Copyright
TODO
