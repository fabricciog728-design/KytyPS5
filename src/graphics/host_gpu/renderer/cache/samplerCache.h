#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SAMPLERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SAMPLERCACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <cstddef>
#include <unordered_map>

namespace Libs::Graphics {

struct GraphicContext;

class SamplerCache {
public:
	explicit SamplerCache(GraphicContext& graphics): m_graphics(graphics) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		m_samplers.reserve(128);
	}
	~SamplerCache();
	KYTY_CLASS_NO_COPY(SamplerCache);

	vk::Sampler GetSampler(const ShaderSamplerResource& r);

private:
	using SamplerKey = std::array<uint32_t, 4>;

	struct SamplerEntry {
		vk::Sampler sampler = nullptr;
		uint64_t    tick    = 0;
	};

	struct SamplerKeyHash {
		std::size_t operator()(const SamplerKey& key) const {
			std::size_t hash = 0;
			for (auto value: key) {
				hash ^= static_cast<std::size_t>(value) +
				        static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
				        (hash >> 2u);
			}
			return hash;
		}
	};

	GraphicContext&                                             m_graphics;
	Common::Mutex                                               m_mutex;
	std::unordered_map<SamplerKey, SamplerEntry, SamplerKeyHash> m_samplers;
	// Lookup counter for age-based eviction. Active samplers are touched on
	// every draw, so a large age margin can never catch an in-flight sampler.
	uint64_t m_tick = 0;

	// Destroys entries unused for the last kSamplerGcAge lookups. Only runs
	// past kSamplerGcSize entries, so steady-state games pay nothing.
	void CollectStale(uint64_t age);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SAMPLERCACHE_H_
