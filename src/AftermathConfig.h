#pragma once

#include "RHIBuildConfig.h"

// USE_AFTERMATH is the legacy switch used throughout DX12Backend / D3D12Helpers
// for compatibility. CORONA_HAS_AFTERMATH from RHIBuildConfig.h is the new
// canonical name; keep them in sync.
#ifndef USE_AFTERMATH
#  define USE_AFTERMATH CORONA_HAS_AFTERMATH
#endif
