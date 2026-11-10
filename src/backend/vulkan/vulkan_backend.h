// HRL native Vulkan backend - internal entry point (not part of the public API).
#pragma once

#include "../../core/backend_vtable.h"

// Backend table used by the frontend when HRL_Init(HRL_VULKAN) is called.
HRL_vtable GetVulkanBackend();
