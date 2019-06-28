# DX12 & DXR test framework
This project is for dx12 & dxr learning. Quite not orgnized for now. I am focussing about the actual working implementation of each tech issues(resource binding, dynamic descriotpr allocation, MT, etc....)

## DX12
* Modified from ms dynamicindexing tutorial sln.(Almost nothing left now, will change sln name soon)
* Helper classes for various resource allocation.
* Simple resource binding interface.
* Dynamic descriptor allocation strategy.
* Multi threaded mesh rendering.
* Most implementations are in DummyRHI_DX12.h/cpp, will be renamed someday.

## DXR
* Will implement most of advanced lighting effects(shadow, reflection, ao) using raytracing for learning purpose.

## Third-party libs
* enkiTS
* glm

## Known issues
* Cannot reuse RTPipelineStateObject
	* Constant buffer is being overwritten.
	* Similar issue with mesh cbv.
		* draw index is used for mesh draw to allocate seperated cbv.
		* draw index is not intuitive for PSO reuse usage.