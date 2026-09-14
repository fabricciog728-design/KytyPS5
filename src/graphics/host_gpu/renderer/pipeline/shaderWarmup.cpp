#include "graphics/host_gpu/renderer/pipeline/shaderWarmup.h"

#include "common/file.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace Libs::Graphics::ShaderWarmup {
namespace {

inline constexpr char kMagic[8] = {'K', 'Y', 'T', 'Y', 'S', 'W', 'C', '1'};
inline constexpr char kMagicV2[8] = {'K', 'Y', 'T', 'Y', 'S', 'W', 'C', '2'};

uint64_t Fnv1a64(const void* data, size_t size, uint64_t hash = 1469598103934665603ull) noexcept {
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; i++) {
		hash ^= static_cast<uint64_t>(bytes[i]);
		hash *= 1099511628211ull;
	}
	return hash;
}

void PutU8(std::vector<uint8_t>& out, uint8_t value) {
	out.push_back(value);
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
	for (int i = 0; i < 4; i++) {
		out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
	}
}

void PutU64(std::vector<uint8_t>& out, uint64_t value) {
	for (int i = 0; i < 8; i++) {
		out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
	}
}

void PutBytes(std::vector<uint8_t>& out, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	out.insert(out.end(), bytes, bytes + size);
}

void PutU32Vec(std::vector<uint8_t>& out, const std::vector<uint32_t>& values) {
	PutU32(out, static_cast<uint32_t>(values.size()));
	if (!values.empty()) {
		PutBytes(out, values.data(), values.size() * sizeof(uint32_t));
	}
}

void PutBool(std::vector<uint8_t>& out, bool value) {
	PutU8(out, value ? 1u : 0u);
}

void PutI32(std::vector<uint8_t>& out, int32_t value) {
	PutU32(out, static_cast<uint32_t>(value));
}

void PutI64(std::vector<uint8_t>& out, int64_t value) {
	uint64_t words = 0;
	std::memcpy(&words, &value, sizeof(words));
	PutU64(out, words);
}

template <typename E>
void PutEnum(std::vector<uint8_t>& out, E value) {
	PutU32(out, static_cast<uint32_t>(value));
}

void PutString(std::vector<uint8_t>& out, const std::string& value) {
	PutU32(out, static_cast<uint32_t>(value.size()));
	PutBytes(out, value.data(), value.size());
}

class Reader {
public:
	Reader(const uint8_t* data, size_t size): data_(data), size_(size) {}

	bool ReadU8(uint8_t& value) {
		if (Remaining() < 1) {
			return false;
		}
		value = data_[pos_++];
		return true;
	}

	bool ReadU32(uint32_t& value) {
		if (Remaining() < 4) {
			return false;
		}
		value = static_cast<uint32_t>(data_[pos_]) |
		        (static_cast<uint32_t>(data_[pos_ + 1]) << 8u) |
		        (static_cast<uint32_t>(data_[pos_ + 2]) << 16u) |
		        (static_cast<uint32_t>(data_[pos_ + 3]) << 24u);
		pos_ += 4;
		return true;
	}

	bool ReadU64(uint64_t& value) {
		if (Remaining() < 8) {
			return false;
		}
		value = 0;
		for (int i = 0; i < 8; i++) {
			value |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8u);
		}
		pos_ += 8;
		return true;
	}

	bool ReadBytes(void* dst, size_t size) {
		if (Remaining() < size) {
			return false;
		}
		std::memcpy(dst, data_ + pos_, size);
		pos_ += size;
		return true;
	}

	bool ReadString(std::string& value) {
		uint32_t size = 0;
		if (!ReadU32(size) || Remaining() < size) {
			return false;
		}
		value.assign(reinterpret_cast<const char*>(data_ + pos_), size);
		pos_ += size;
		return true;
	}

	bool ReadU32Vec(std::vector<uint32_t>& values, uint32_t max_words) {
		uint32_t count = 0;
		if (!ReadU32(count) || count > max_words) {
			return false;
		}
		values.resize(count);
		if (count != 0 && !ReadBytes(values.data(), static_cast<size_t>(count) * 4u)) {
			values.clear();
			return false;
		}
		return true;
	}

	bool ReadBool(bool& value) {
		uint8_t byte = 0;
		if (!ReadU8(byte)) {
			return false;
		}
		value = byte != 0;
		return true;
	}

	bool ReadI32(int32_t& value) {
		uint32_t words = 0;
		if (!ReadU32(words)) {
			return false;
		}
		value = static_cast<int32_t>(words);
		return true;
	}

	bool ReadI64(int64_t& value) {
		uint64_t words = 0;
		if (!ReadU64(words)) {
			return false;
		}
		std::memcpy(&value, &words, sizeof(value));
		return true;
	}

	template <typename E>
	bool ReadEnum(E& value) {
		uint32_t words = 0;
		if (!ReadU32(words)) {
			return false;
		}
		value = static_cast<E>(words);
		return true;
	}

	bool ReadString(std::string& value, uint32_t max_bytes) {
		uint32_t size = 0;
		if (!ReadU32(size) || size > max_bytes || Remaining() < size) {
			return false;
		}
		value.assign(reinterpret_cast<const char*>(data_ + pos_), size);
		pos_ += size;
		return true;
	}

	[[nodiscard]] size_t Remaining() const { return size_ - pos_; }

private:
	const uint8_t* data_ = nullptr;
	size_t         size_ = 0;
	size_t         pos_  = 0;
};

bool ValidRecord(const Record& record) {
	if (record.code_words == 0 || record.code_words > kMaxCodeWords) {
		return false;
	}
	if (record.code_addr == 0) {
		return false;
	}
	if (record.code_addr + static_cast<uint64_t>(record.code_words) * 4u < record.code_addr) {
		return false;
	}
	if (record.user_data.size() > kMaxUserDataWords) {
		return false;
	}
	if (record.back_code.size() > kMaxBackCodeWords) {
		return false;
	}
	if (record.input_kind > kInputCompute) {
		return false;
	}
	if (record.input_size == 0 || record.input_size > kMaxInputBytes) {
		return false;
	}
	if (record.input_head.size() + record.input_tail.size() > record.input_size) {
		return false;
	}
	return true;
}

void SerializeRecord(const Record& record, std::vector<uint8_t>& out) {
	PutU8(out, record.stage);
	PutU64(out, record.hash);
	PutU64(out, record.code_addr);
	PutU32(out, record.code_words);
	PutU32Vec(out, record.user_data);
	PutU32Vec(out, record.back_code);
	PutU32(out, record.wave_size);
	PutU32(out, record.user_data_base);
	PutU32(out, record.scratch_dwords);
	PutU32(out, record.push_cursor);
	PutU8(out, record.input_kind);
	PutU32(out, record.input_size);
	PutU32(out, static_cast<uint32_t>(record.input_head.size()));
	PutBytes(out, record.input_head.data(), record.input_head.size());
	PutU32(out, static_cast<uint32_t>(record.input_tail.size()));
	PutBytes(out, record.input_tail.data(), record.input_tail.size());
}

bool DeserializeRecord(Reader& in, Record& record) {
	uint32_t head_size = 0;
	uint32_t tail_size = 0;
	if (!in.ReadU8(record.stage) || !in.ReadU64(record.hash) || !in.ReadU64(record.code_addr) ||
	    !in.ReadU32(record.code_words) || !in.ReadU32Vec(record.user_data, kMaxUserDataWords) ||
	    !in.ReadU32Vec(record.back_code, kMaxBackCodeWords) || !in.ReadU32(record.wave_size) ||
	    !in.ReadU32(record.user_data_base) || !in.ReadU32(record.scratch_dwords) ||
	    !in.ReadU32(record.push_cursor) || !in.ReadU8(record.input_kind) ||
	    !in.ReadU32(record.input_size) || !in.ReadU32(head_size) || head_size > kMaxInputBytes) {
		return false;
	}
	record.input_head.resize(head_size);
	if (head_size != 0 && !in.ReadBytes(record.input_head.data(), head_size)) {
		return false;
	}
	if (!in.ReadU32(tail_size) || tail_size > kMaxInputBytes) {
		return false;
	}
	record.input_tail.resize(tail_size);
	if (tail_size != 0 && !in.ReadBytes(record.input_tail.data(), tail_size)) {
		return false;
	}
	return ValidRecord(record);
}

// Decode caps: generous multiples of the compiler's own Max* constants, only
// meant to stop corrupt files from ballooning RAM.
inline constexpr uint32_t kMaxSpecBuffers  = 256;
inline constexpr uint32_t kMaxSpecImages   = 512;
inline constexpr uint32_t kMaxInfoBuffers  = 128;
inline constexpr uint32_t kMaxInfoImages   = 256;
inline constexpr uint32_t kMaxInfoSamplers = 128;
inline constexpr uint32_t kMaxInfoPairs    = 256;
inline constexpr uint32_t kMaxInfoInputs   = 256;
inline constexpr uint32_t kMaxInfoOutputs  = 256;
inline constexpr uint32_t kMaxIndirectU32  = 4096;
inline constexpr uint32_t kMaxUdRegs       = 1024;
inline constexpr uint32_t kMaxBindings     = 256;
inline constexpr uint32_t kMaxBoundRes     = 1024;
inline constexpr uint32_t kMaxDebugName    = 4096;

bool ValidVariant(const SpvVariant& variant) {
	return variant.spec.size() <= kMaxSpecBytes && variant.info.size() <= kMaxInfoBytes &&
	       variant.spirv.size() <= kMaxSpirvWords;
}

void SerializeVariant(const SpvVariant& variant, std::vector<uint8_t>& out) {
	PutU32(out, static_cast<uint32_t>(variant.spec.size()));
	PutBytes(out, variant.spec.data(), variant.spec.size());
	PutU32(out, static_cast<uint32_t>(variant.info.size()));
	PutBytes(out, variant.info.data(), variant.info.size());
	PutU32(out, static_cast<uint32_t>(variant.spirv.size()));
	PutBytes(out, variant.spirv.data(), variant.spirv.size() * 4u);
}

bool DeserializeVariant(Reader& in, SpvVariant& variant) {
	uint32_t spec_size  = 0;
	uint32_t info_size  = 0;
	uint32_t spirv_size = 0;
	if (!in.ReadU32(spec_size) || spec_size > kMaxSpecBytes) {
		return false;
	}
	variant.spec.resize(spec_size);
	if (spec_size != 0 && !in.ReadBytes(variant.spec.data(), spec_size)) {
		return false;
	}
	if (!in.ReadU32(info_size) || info_size > kMaxInfoBytes) {
		return false;
	}
	variant.info.resize(info_size);
	if (info_size != 0 && !in.ReadBytes(variant.info.data(), info_size)) {
		return false;
	}
	if (!in.ReadU32(spirv_size) || spirv_size > kMaxSpirvWords) {
		return false;
	}
	variant.spirv.resize(spirv_size);
	if (spirv_size != 0 &&
	    !in.ReadBytes(variant.spirv.data(), static_cast<size_t>(spirv_size) * 4u)) {
		return false;
	}
	return ValidVariant(variant);
}

void SerializeRecordV2(const Record& record, std::vector<uint8_t>& out) {
	SerializeRecord(record, out);
	PutU32(out, static_cast<uint32_t>(record.variants.size()));
	for (const auto& variant : record.variants) {
		SerializeVariant(variant, out);
	}
}

bool DeserializeRecordV2(Reader& in, Record& record) {
	uint32_t variant_count = 0;
	if (!DeserializeRecord(in, record)) {
		return false;
	}
	record.variants.clear();
	if (!in.ReadU32(variant_count) || variant_count > kMaxVariantsPerRecord) {
		return false;
	}
	record.variants.reserve(variant_count);
	for (uint32_t i = 0; i < variant_count; i++) {
		SpvVariant variant;
		if (!DeserializeVariant(in, variant)) {
			return false;
		}
		record.variants.push_back(std::move(variant));
	}
	return ValidRecord(record);
}

bool ValidRecordV2(const Record& record) {
	if (!ValidRecord(record) || record.variants.size() > kMaxVariantsPerRecord) {
		return false;
	}
	for (const auto& variant : record.variants) {
		if (!ValidVariant(variant)) {
			return false;
		}
	}
	return true;
}

using IRSpec = ShaderRecompiler::IR::ResourceSpecialization;
using IRInfo = ShaderRecompiler::IR::CompiledShaderInfo;

void EncodeSpecBuffer(const IRSpec::Buffer& buffer, std::vector<uint8_t>& out) {
	PutU32(out, buffer.packed_stride);
	PutEnum(out, buffer.descriptor_format);
	PutU32(out, buffer.descriptor_swizzle);
	PutBool(out, buffer.byte_base_offset);
}

bool DecodeSpecBuffer(Reader& in, IRSpec::Buffer& buffer) {
	return in.ReadU32(buffer.packed_stride) && in.ReadEnum(buffer.descriptor_format) &&
	       in.ReadU32(buffer.descriptor_swizzle) && in.ReadBool(buffer.byte_base_offset);
}

void EncodeSpecImage(const IRSpec::Image& image, std::vector<uint8_t>& out) {
	PutEnum(out, image.numeric_class);
	PutEnum(out, image.dimension);
	PutU32(out, image.mip_count);
	PutEnum(out, image.conversion_format);
	PutU32(out, image.shader_swizzle);
	PutU32(out, image.indirect_root);
	PutU32(out, image.indirect_mapping_offset);
	PutU32(out, image.indirect_search_iterations);
	PutBool(out, image.cube);
	PutBool(out, image.fmask);
}

bool DecodeSpecImage(Reader& in, IRSpec::Image& image) {
	return in.ReadEnum(image.numeric_class) && in.ReadEnum(image.dimension) &&
	       in.ReadU32(image.mip_count) && in.ReadEnum(image.conversion_format) &&
	       in.ReadU32(image.shader_swizzle) && in.ReadU32(image.indirect_root) &&
	       in.ReadU32(image.indirect_mapping_offset) &&
	       in.ReadU32(image.indirect_search_iterations) && in.ReadBool(image.cube) &&
	       in.ReadBool(image.fmask);
}

void EncodeBdaWord(const ShaderRecompiler::IR::BdaReadWord& word, std::vector<uint8_t>& out) {
	PutU8(out, static_cast<uint8_t>(word.kind));
	PutU32(out, word.value);
}

bool DecodeBdaWord(Reader& in, ShaderRecompiler::IR::BdaReadWord& word) {
	uint8_t kind = 0;
	if (!in.ReadU8(kind) || !in.ReadU32(word.value)) {
		return false;
	}
	word.kind = static_cast<ShaderRecompiler::IR::BdaReadWord::Kind>(kind);
	return true;
}

void EncodeBdaSpan(const ShaderRecompiler::IR::BdaReadSpan& span, std::vector<uint8_t>& out) {
	EncodeBdaWord(span.low, out);
	EncodeBdaWord(span.high, out);
	PutI64(out, span.begin);
	PutI64(out, span.end);
	PutBool(out, span.scalar_base);
}

bool DecodeBdaSpan(Reader& in, ShaderRecompiler::IR::BdaReadSpan& span) {
	return DecodeBdaWord(in, span.low) && DecodeBdaWord(in, span.high) &&
	       in.ReadI64(span.begin) && in.ReadI64(span.end) && in.ReadBool(span.scalar_base);
}

void EncodeBufferResource(const ShaderRecompiler::IR::BufferResource& buffer,
                          std::vector<uint8_t>& out) {
	PutU32(out, buffer.source);
	PutU32(out, buffer.first_use_pc);
	PutU32(out, buffer.max_byte_extent);
	PutU32(out, buffer.packed_stride);
	PutEnum(out, buffer.descriptor_format);
	PutU32(out, buffer.descriptor_swizzle);
	PutU32(out, buffer.image_alias);
	PutBool(out, buffer.read);
	PutBool(out, buffer.written);
	PutBool(out, buffer.atomic);
	PutBool(out, buffer.formatted);
	PutBool(out, buffer.scalar);
	PutBool(out, buffer.byte_base_offset);
}

bool DecodeBufferResource(Reader& in, ShaderRecompiler::IR::BufferResource& buffer) {
	return in.ReadU32(buffer.source) && in.ReadU32(buffer.first_use_pc) &&
	       in.ReadU32(buffer.max_byte_extent) && in.ReadU32(buffer.packed_stride) &&
	       in.ReadEnum(buffer.descriptor_format) && in.ReadU32(buffer.descriptor_swizzle) &&
	       in.ReadU32(buffer.image_alias) && in.ReadBool(buffer.read) &&
	       in.ReadBool(buffer.written) && in.ReadBool(buffer.atomic) &&
	       in.ReadBool(buffer.formatted) && in.ReadBool(buffer.scalar) &&
	       in.ReadBool(buffer.byte_base_offset);
}

void EncodeImageResource(const ShaderRecompiler::IR::ImageResource& image,
                         std::vector<uint8_t>& out) {
	PutU32(out, image.source);
	PutU32(out, image.first_use_pc);
	PutEnum(out, image.resource_class);
	PutEnum(out, image.numeric_class);
	PutEnum(out, image.dimension);
	PutEnum(out, image.mip_mode);
	PutU32(out, image.mip_count);
	PutEnum(out, image.conversion_format);
	PutU32(out, image.shader_swizzle);
	PutBool(out, image.read);
	PutBool(out, image.written);
	PutBool(out, image.atomic);
	PutBool(out, image.depth_compare);
	PutBool(out, image.cube);
	PutBool(out, image.r128);
	PutU32(out, image.indirect_root);
	PutU32(out, image.indirect_mapping_offset);
	PutU32(out, image.indirect_search_iterations);
	PutU32(out, static_cast<uint32_t>(image.indirect_resources.size()));
	PutBytes(out, image.indirect_resources.data(),
	         image.indirect_resources.size() * sizeof(uint32_t));
}

bool DecodeImageResource(Reader& in, ShaderRecompiler::IR::ImageResource& image) {
	uint32_t indirect_count = 0;
	if (!(in.ReadU32(image.source) && in.ReadU32(image.first_use_pc) &&
	      in.ReadEnum(image.resource_class) && in.ReadEnum(image.numeric_class) &&
	      in.ReadEnum(image.dimension) && in.ReadEnum(image.mip_mode) &&
	      in.ReadU32(image.mip_count) && in.ReadEnum(image.conversion_format) &&
	      in.ReadU32(image.shader_swizzle) && in.ReadBool(image.read) &&
	      in.ReadBool(image.written) && in.ReadBool(image.atomic) &&
	      in.ReadBool(image.depth_compare) && in.ReadBool(image.cube) && in.ReadBool(image.r128) &&
	      in.ReadU32(image.indirect_root) && in.ReadU32(image.indirect_mapping_offset) &&
	      in.ReadU32(image.indirect_search_iterations) && in.ReadU32(indirect_count)) ||
	    indirect_count > kMaxIndirectU32) {
		return false;
	}
	image.indirect_resources.resize(indirect_count);
	return indirect_count == 0 ||
	       in.ReadBytes(image.indirect_resources.data(),
	                     static_cast<size_t>(indirect_count) * 4u);
}

void EncodeSamplerResource(const ShaderRecompiler::IR::SamplerResource& sampler,
                           std::vector<uint8_t>& out) {
	PutU32(out, sampler.source);
	PutU32(out, sampler.first_use_pc);
	PutBool(out, sampler.force_point_filtering);
	PutBool(out, sampler.depth_compare);
}

bool DecodeSamplerResource(Reader& in, ShaderRecompiler::IR::SamplerResource& sampler) {
	return in.ReadU32(sampler.source) && in.ReadU32(sampler.first_use_pc) &&
	       in.ReadBool(sampler.force_point_filtering) && in.ReadBool(sampler.depth_compare);
}

void EncodeSampledPair(const ShaderRecompiler::IR::SampledResourcePair& pair,
                       std::vector<uint8_t>& out) {
	PutU32(out, pair.image);
	PutU32(out, pair.sampler);
	PutU32(out, pair.first_use_pc);
}

bool DecodeSampledPair(Reader& in, ShaderRecompiler::IR::SampledResourcePair& pair) {
	return in.ReadU32(pair.image) && in.ReadU32(pair.sampler) && in.ReadU32(pair.first_use_pc);
}

void EncodeStageInput(const ShaderRecompiler::IR::StageInput& input, std::vector<uint8_t>& out) {
	PutEnum(out, input.kind);
	PutU32(out, input.location);
	PutU32(out, input.component_count);
	PutString(out, input.debug_name);
	PutBool(out, input.per_vertex);
}

bool DecodeStageInput(Reader& in, ShaderRecompiler::IR::StageInput& input) {
	return in.ReadEnum(input.kind) && in.ReadU32(input.location) &&
	       in.ReadU32(input.component_count) && in.ReadString(input.debug_name, kMaxDebugName) &&
	       in.ReadBool(input.per_vertex);
}

void EncodeStageOutput(const ShaderRecompiler::IR::StageOutput& output,
                       std::vector<uint8_t>& out) {
	PutEnum(out, output.kind);
	PutU32(out, output.index);
	PutU32(out, output.location);
	PutString(out, output.debug_name);
}

bool DecodeStageOutput(Reader& in, ShaderRecompiler::IR::StageOutput& output) {
	return in.ReadEnum(output.kind) && in.ReadU32(output.index) &&
	       in.ReadU32(output.location) && in.ReadString(output.debug_name, kMaxDebugName);
}

void EncodeBinding(const ShaderRecompiler::IR::DescriptorBinding& binding,
                   std::vector<uint8_t>& out) {
	PutEnum(out, binding.kind);
	PutU32(out, static_cast<uint32_t>(binding.resources.size()));
	PutBytes(out, binding.resources.data(), binding.resources.size() * sizeof(uint32_t));
}

bool DecodeBinding(Reader& in, ShaderRecompiler::IR::DescriptorBinding& binding) {
	uint32_t count = 0;
	if (!in.ReadEnum(binding.kind) || !in.ReadU32(count) || count > kMaxBoundRes) {
		return false;
	}
	binding.resources.resize(count);
	return count == 0 ||
	       in.ReadBytes(binding.resources.data(), static_cast<size_t>(count) * 4u);
}

} // namespace

// The four functions below are public API (declared in shaderWarmup.h) and
// must live outside the anonymous namespace: internal linkage would make them
// invisible to pipelineCache.cpp (undefined references at link).

void EncodeSpecialization(const IRSpec& spec, std::vector<uint8_t>& out) {
	PutU32(out, static_cast<uint32_t>(spec.buffers.size()));
	for (const auto& buffer : spec.buffers) {
		EncodeSpecBuffer(buffer, out);
	}
	PutU32(out, static_cast<uint32_t>(spec.images.size()));
	for (const auto& image : spec.images) {
		EncodeSpecImage(image, out);
	}
	PutBool(out, spec.enable_lod_stats);
}

bool DecodeSpecialization(const uint8_t* data, size_t size, IRSpec& spec) {
	if (data == nullptr && size != 0) {
		return false;
	}
	Reader   in(data, size);
	uint32_t buffer_count = 0;
	uint32_t image_count  = 0;
	if (!in.ReadU32(buffer_count) || buffer_count > kMaxSpecBuffers) {
		return false;
	}
	spec.buffers.clear();
	for (uint32_t i = 0; i < buffer_count; i++) {
		IRSpec::Buffer buffer;
		if (!DecodeSpecBuffer(in, buffer)) {
			return false;
		}
		spec.buffers.push_back(buffer);
	}
	if (!in.ReadU32(image_count) || image_count > kMaxSpecImages) {
		return false;
	}
	spec.images.clear();
	for (uint32_t i = 0; i < image_count; i++) {
		IRSpec::Image image;
		if (!DecodeSpecImage(in, image)) {
			return false;
		}
		spec.images.push_back(image);
	}
	return in.ReadBool(spec.enable_lod_stats) && in.Remaining() == 0;
}

void EncodeCompiledInfo(const IRInfo& info, std::vector<uint8_t>& out) {
	PutU32(out, static_cast<uint32_t>(info.bda_read_plan.spans.size()));
	for (const auto& span : info.bda_read_plan.spans) {
		EncodeBdaSpan(span, out);
	}
	PutBool(out, info.bda_read_plan.complete);
	PutEnum(out, info.stage);
	PutU64(out, info.shader_hash);
	PutU32(out, info.wave_size);
	PutU32(out, info.user_data_base);
	PutU32(out, info.user_data_count);
	PutU32(out, info.scratch_dwords);
	PutU32(out, info.param_export_mask);
	PutU32(out, static_cast<uint32_t>(info.info.buffers.size()));
	for (const auto& buffer : info.info.buffers) {
		EncodeBufferResource(buffer, out);
	}
	PutU32(out, static_cast<uint32_t>(info.info.images.size()));
	for (const auto& image : info.info.images) {
		EncodeImageResource(image, out);
	}
	PutU32(out, static_cast<uint32_t>(info.info.samplers.size()));
	for (const auto& sampler : info.info.samplers) {
		EncodeSamplerResource(sampler, out);
	}
	PutU32(out, static_cast<uint32_t>(info.info.sampled_pairs.size()));
	for (const auto& pair : info.info.sampled_pairs) {
		EncodeSampledPair(pair, out);
	}
	PutU32(out, static_cast<uint32_t>(info.info.inputs.size()));
	for (const auto& input : info.info.inputs) {
		EncodeStageInput(input, out);
	}
	PutU32(out, static_cast<uint32_t>(info.info.outputs.size()));
	for (const auto& output : info.info.outputs) {
		EncodeStageOutput(output, out);
	}
	for (const auto component : info.info.vertex_fetch_components) {
		PutU8(out, component);
	}
	PutI32(out, info.info.vertex_offset_sgpr);
	PutI32(out, info.info.instance_offset_sgpr);
	PutBool(out, info.info.has_bitwise_xor);
	PutBool(out, info.info.uses_dma);
	PutU32(out, info.bindings.push_data_start_dword);
	PutU32(out, info.bindings.memory_offset_dword);
	PutU32(out, info.bindings.memory_offset_count);
	PutU32(out, info.bindings.lod_stats_count);
	PutU32(out, static_cast<uint32_t>(info.bindings.user_data_registers.size()));
	PutBytes(out, info.bindings.user_data_registers.data(),
	         info.bindings.user_data_registers.size() * sizeof(uint32_t));
	PutU32(out, static_cast<uint32_t>(info.bindings.descriptors.size()));
	for (const auto& binding : info.bindings.descriptors) {
		EncodeBinding(binding, out);
	}
}

bool DecodeCompiledInfo(const uint8_t* data, size_t size, IRInfo& info) {
	if (data == nullptr && size != 0) {
		return false;
	}
	Reader   in(data, size);
	uint32_t span_count = 0;
	if (!in.ReadU32(span_count) ||
	    span_count > ShaderRecompiler::IR::BdaReadPlan::Capacity) {
		return false;
	}
	info.bda_read_plan.spans.clear();
	for (uint32_t i = 0; i < span_count; i++) {
		ShaderRecompiler::IR::BdaReadSpan span;
		if (!DecodeBdaSpan(in, span)) {
			return false;
		}
		info.bda_read_plan.spans.push_back(span);
	}
	uint32_t buffer_count = 0;
	uint32_t image_count  = 0;
	uint32_t sampler_count = 0;
	uint32_t pair_count   = 0;
	uint32_t input_count  = 0;
	uint32_t output_count = 0;
	uint32_t udreg_count  = 0;
	uint32_t binding_count = 0;
	if (!(in.ReadBool(info.bda_read_plan.complete) && in.ReadEnum(info.stage) &&
	      in.ReadU64(info.shader_hash) && in.ReadU32(info.wave_size) &&
	      in.ReadU32(info.user_data_base) && in.ReadU32(info.user_data_count) &&
	      in.ReadU32(info.scratch_dwords) && in.ReadU32(info.param_export_mask) &&
	      in.ReadU32(buffer_count) && buffer_count <= kMaxInfoBuffers)) {
		return false;
	}
	info.info.buffers.clear();
	for (uint32_t i = 0; i < buffer_count; i++) {
		ShaderRecompiler::IR::BufferResource buffer;
		if (!DecodeBufferResource(in, buffer)) {
			return false;
		}
		info.info.buffers.push_back(std::move(buffer));
	}
	if (!in.ReadU32(image_count) || image_count > kMaxInfoImages) {
		return false;
	}
	info.info.images.clear();
	for (uint32_t i = 0; i < image_count; i++) {
		ShaderRecompiler::IR::ImageResource image;
		if (!DecodeImageResource(in, image)) {
			return false;
		}
		info.info.images.push_back(std::move(image));
	}
	if (!in.ReadU32(sampler_count) || sampler_count > kMaxInfoSamplers) {
		return false;
	}
	info.info.samplers.clear();
	for (uint32_t i = 0; i < sampler_count; i++) {
		ShaderRecompiler::IR::SamplerResource sampler;
		if (!DecodeSamplerResource(in, sampler)) {
			return false;
		}
		info.info.samplers.push_back(std::move(sampler));
	}
	if (!in.ReadU32(pair_count) || pair_count > kMaxInfoPairs) {
		return false;
	}
	info.info.sampled_pairs.clear();
	for (uint32_t i = 0; i < pair_count; i++) {
		ShaderRecompiler::IR::SampledResourcePair pair;
		if (!DecodeSampledPair(in, pair)) {
			return false;
		}
		info.info.sampled_pairs.push_back(std::move(pair));
	}
	if (!in.ReadU32(input_count) || input_count > kMaxInfoInputs) {
		return false;
	}
	info.info.inputs.clear();
	for (uint32_t i = 0; i < input_count; i++) {
		ShaderRecompiler::IR::StageInput input;
		if (!DecodeStageInput(in, input)) {
			return false;
		}
		info.info.inputs.push_back(std::move(input));
	}
	if (!in.ReadU32(output_count) || output_count > kMaxInfoOutputs) {
		return false;
	}
	info.info.outputs.clear();
	for (uint32_t i = 0; i < output_count; i++) {
		ShaderRecompiler::IR::StageOutput output;
		if (!DecodeStageOutput(in, output)) {
			return false;
		}
		info.info.outputs.push_back(std::move(output));
	}
	for (auto& component : info.info.vertex_fetch_components) {
		uint8_t value = 0;
		if (!in.ReadU8(value)) {
			return false;
		}
		component = value;
	}
	if (!(in.ReadI32(info.info.vertex_offset_sgpr) && in.ReadI32(info.info.instance_offset_sgpr) &&
	      in.ReadBool(info.info.has_bitwise_xor) && in.ReadBool(info.info.uses_dma) &&
	      in.ReadU32(info.bindings.push_data_start_dword) &&
	      in.ReadU32(info.bindings.memory_offset_dword) &&
	      in.ReadU32(info.bindings.memory_offset_count) &&
	      in.ReadU32(info.bindings.lod_stats_count) && in.ReadU32(udreg_count)) ||
	    udreg_count > kMaxUdRegs) {
		return false;
	}
	info.bindings.user_data_registers.resize(udreg_count);
	if (udreg_count != 0 &&
	    !in.ReadBytes(info.bindings.user_data_registers.data(),
	                  static_cast<size_t>(udreg_count) * 4u)) {
		return false;
	}
	if (!in.ReadU32(binding_count) || binding_count > kMaxBindings) {
		return false;
	}
	info.bindings.descriptors.clear();
	for (uint32_t i = 0; i < binding_count; i++) {
		ShaderRecompiler::IR::DescriptorBinding binding;
		if (!DecodeBinding(in, binding)) {
			return false;
		}
		info.bindings.descriptors.push_back(std::move(binding));
	}
	return in.Remaining() == 0;
}

uint64_t RecordKey(const Record& record) noexcept {
	uint64_t hash = Fnv1a64(&record.stage, sizeof(record.stage));
	hash          = Fnv1a64(&record.hash, sizeof(record.hash), hash);
	hash          = Fnv1a64(&record.code_words, sizeof(record.code_words), hash);
	hash          = Fnv1a64(&record.wave_size, sizeof(record.wave_size), hash);
	hash          = Fnv1a64(&record.user_data_base, sizeof(record.user_data_base), hash);
	hash          = Fnv1a64(&record.scratch_dwords, sizeof(record.scratch_dwords), hash);
	hash          = Fnv1a64(&record.push_cursor, sizeof(record.push_cursor), hash);
	hash          = Fnv1a64(&record.input_kind, sizeof(record.input_kind), hash);
	hash          = Fnv1a64(&record.input_size, sizeof(record.input_size), hash);
	if (!record.user_data.empty()) {
		hash = Fnv1a64(record.user_data.data(), record.user_data.size() * 4u, hash);
	}
	if (!record.back_code.empty()) {
		hash = Fnv1a64(record.back_code.data(), record.back_code.size() * 4u, hash);
	}
	if (!record.input_head.empty()) {
		hash = Fnv1a64(record.input_head.data(), record.input_head.size(), hash);
	}
	if (!record.input_tail.empty()) {
		hash = Fnv1a64(record.input_tail.data(), record.input_tail.size(), hash);
	}
	return hash;
}

bool SaveFile(const std::filesystem::path& path, const std::string& git_revision,
              const std::string& gpu_signature, const std::vector<Record>& records) {
	if (path.empty() || git_revision.empty() || gpu_signature.empty()) {
		return false;
	}
	std::vector<uint8_t> out;
	out.reserve(1024);
	PutBytes(out, kMagicV2, sizeof(kMagicV2));
	PutU32(out, static_cast<uint32_t>(git_revision.size()));
	PutBytes(out, git_revision.data(), git_revision.size());
	PutU32(out, static_cast<uint32_t>(gpu_signature.size()));
	PutBytes(out, gpu_signature.data(), gpu_signature.size());
	uint32_t valid = 0;
	for (const auto& record : records) {
		if (ValidRecordV2(record)) {
			valid++;
		}
	}
	PutU32(out, valid);
	for (const auto& record : records) {
		if (ValidRecordV2(record)) {
			SerializeRecordV2(record, out);
		}
	}
	PutU64(out, Fnv1a64(out.data() + sizeof(kMagicV2), out.size() - sizeof(kMagicV2)));
	if (out.size() > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	if (path.has_parent_path() && !Common::File::CreateDirectories(path.parent_path())) {
		return false;
	}
	auto temp_path = path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     written = 0;
	if (file.Create(temp_path)) {
		file.Write(out.data(), static_cast<uint32_t>(out.size()), &written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (written != out.size() || !flushed || !Common::File::RenameFile(temp_path, path)) {
		return false;
	}
	return true;
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
	data.clear();
	if (!Common::File::IsFileExisting(path)) {
		return false;
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return false;
	}
	const uint64_t size = file.Size();
	// magic + rev len + sig len + count + trailing hash, minimum.
	if (size < sizeof(kMagicV2) + 4u + 4u + 4u + 8u || size > kMaxFileBytes) {
		return false;
	}
	data.resize(static_cast<size_t>(size));
	uint32_t got = 0;
	file.Read(data.data(), static_cast<uint32_t>(size), &got);
	file.Close();
	return got == size;
}

bool VerifyTrailer(const std::vector<uint8_t>& data) {
	Reader   check(data.data() + data.size() - 8u, 8u);
	uint64_t stored = 0;
	return check.ReadU64(stored) &&
	       Fnv1a64(data.data() + 8u, data.size() - 16u) == stored;
}

bool ParseRecords(Reader& in, const std::string& git_revision, const std::string& gpu_signature,
                  bool v2, std::vector<Record>& records_out) {
	std::string file_rev;
	std::string file_sig;
	uint32_t    count = 0;
	if (!in.ReadString(file_rev) || file_rev != git_revision || !in.ReadString(file_sig) ||
	    file_sig != gpu_signature || !in.ReadU32(count) || count > kMaxRecords) {
		return false;
	}
	std::vector<Record>          records;
	std::unordered_set<uint64_t> seen;
	records.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		Record record;
		if (v2) {
			if (!DeserializeRecordV2(in, record)) {
				return false;
			}
		} else {
			if (!DeserializeRecord(in, record)) {
				return false;
			}
		}
		// Defensive: drop exact duplicates, keep the file usable.
		if (seen.insert(RecordKey(record)).second) {
			records.push_back(std::move(record));
		}
	}
	records_out = std::move(records);
	return true;
}

bool LoadFile(const std::filesystem::path& path, const std::string& git_revision,
              const std::string& gpu_signature, std::vector<Record>& records_out) {
	records_out.clear();
	if (path.empty() || git_revision.empty() || gpu_signature.empty()) {
		return false;
	}
	std::vector<uint8_t> data;
	if (!ReadWholeFile(path, data) || !VerifyTrailer(data)) {
		return false;
	}
	const bool v2 = std::memcmp(data.data(), kMagicV2, sizeof(kMagicV2)) == 0;
	const bool v1 = std::memcmp(data.data(), kMagic, sizeof(kMagic)) == 0;
	if (!v2 && !v1) {
		return false;
	}
	Reader in(data.data() + 8u, data.size() - 16u);
	return ParseRecords(in, git_revision, gpu_signature, v2, records_out);
}

void Recorder::Configure(std::filesystem::path path, std::string git_revision,
                         std::string gpu_signature) {
	path_          = std::move(path);
	git_revision_  = std::move(git_revision);
	gpu_signature_ = std::move(gpu_signature);
	configured_    = !path_.empty() && !git_revision_.empty() && !gpu_signature_.empty();
}

bool Recorder::MergeVariant(Record& existing, SpvVariant variant) {
	if (existing.variants.size() >= kMaxVariantsPerRecord || !ValidVariant(variant)) {
		return false;
	}
	for (const auto& known : existing.variants) {
		if (known.spec == variant.spec) {
			return false;
		}
	}
	existing.variants.push_back(std::move(variant));
	return true;
}

void Recorder::Add(Record record) {
	if (!configured_ || !ValidRecordV2(record)) {
		return;
	}
	const uint64_t key = RecordKey(record);
	const auto     it  = index_.find(key);
	if (it != index_.end()) {
		if (MergeInto(it->second, record) && ++dirty_ >= kFlushEveryRecords) {
			Flush();
		}
		return;
	}
	if (records_.size() >= kMaxRecords) {
		return;
	}
	index_.emplace(key, records_.size());
	records_.push_back(std::move(record));
	if (++dirty_ >= kFlushEveryRecords) {
		Flush();
	}
}

size_t Recorder::Seed(std::vector<Record> loaded) {
	for (auto& record : loaded) {
		if (!configured_ || !ValidRecordV2(record)) {
			continue;
		}
		const uint64_t key = RecordKey(record);
		const auto     it  = index_.find(key);
		if (it != index_.end()) {
			// A live compile beat the seeder to this shader: keep the union.
			// Already-on-disk data needs no dirty flag unless it grew.
			if (MergeInto(it->second, record)) {
				++dirty_;
			}
			continue;
		}
		if (records_.size() >= kMaxRecords) {
			continue;
		}
		index_.emplace(key, records_.size());
		records_.push_back(std::move(record));
	}
	return records_.size();
}

bool Recorder::FetchCopy(size_t index, Record& out) const {
	if (index >= records_.size()) {
		return false;
	}
	out = records_[index];
	return true;
}

bool Recorder::MergeInto(size_t pos, Record& src) {
	// Same shader re-recorded (e.g. a new specialization discovered later):
	// merge unknown variants instead of duplicating the record.
	bool changed = false;
	for (auto& variant : src.variants) {
		changed = MergeVariant(records_[pos], std::move(variant)) || changed;
	}
	return changed;
}

void Recorder::Flush() {
	if (!configured_ || dirty_ == 0) {
		return;
	}
	if (SaveFile(path_, git_revision_, gpu_signature_, records_)) {
		dirty_ = 0;
	}
}

} // namespace Libs::Graphics::ShaderWarmup
