#pragma once
#include <cstddef>
#include <cstdint>
namespace gpu {
struct VideoProcessorCacheUsage { size_t processors=0, output_views=0; uint64_t texture_bytes=0; size_t device_users=0; bool device_loaded=false; };
VideoProcessorCacheUsage VideoProcessorCacheSnapshot();
}
