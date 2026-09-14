#include "graphics/host_gpu/renderer/pipeline/shaderWarmup.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

void Check(bool ok) {
	if (!ok) {
		std::fputs("shader warmup regression\n", stderr);
		std::abort();
	}
}

Libs::Graphics::ShaderWarmup::Record MakeRecord(uint8_t kind, uint64_t hash) {
	using namespace Libs::Graphics::ShaderWarmup;
	Record record;
	record.stage       = 1;
	record.hash        = hash;
	record.code_addr   = 0x10000000u + hash;
	record.code_words  = 64;
	record.user_data   = {1u, 2u, 3u};
	record.back_code   = {};
	record.wave_size   = 64;
	record.push_cursor = 6;
	record.input_kind  = kind;
	record.input_size  = 128;
	record.input_head  = std::vector<uint8_t>(48, static_cast<uint8_t>(kind + 1));
	record.input_tail  = std::vector<uint8_t>(64, static_cast<uint8_t>(kind + 2));
	return record;
}

int main() {
	using namespace Libs::Graphics::ShaderWarmup;
	const auto root =
	    std::filesystem::temp_directory_path() /
	    ("kyty-shader-warmup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	const auto path = root / "warmup.bin";

	std::vector<Record> records;
	records.push_back(MakeRecord(kInputVertex, 0x1111u));
	records.push_back(MakeRecord(kInputPixel, 0x2222u));
	records.push_back(MakeRecord(kInputCompute, 0x3333u));
	// Exact duplicate must be deduplicated by key.
	records.push_back(MakeRecord(kInputVertex, 0x1111u));
	Check(RecordKey(records[0]) == RecordKey(records[3]));
	Check(RecordKey(records[0]) != RecordKey(records[1]));

	Check(SaveFile(path, "rev1", "gpu1", records));
	std::vector<Record> loaded;
	Check(LoadFile(path, "rev1", "gpu1", loaded));
	Check(loaded.size() == 3);
	for (size_t i = 0; i < 3; i++) {
		Check(loaded[i].hash == records[i].hash);
		Check(loaded[i].code_addr == records[i].code_addr);
		Check(loaded[i].user_data == records[i].user_data);
		Check(loaded[i].input_head == records[i].input_head);
		Check(loaded[i].input_tail == records[i].input_tail);
		Check(loaded[i].input_kind == records[i].input_kind);
		Check(loaded[i].push_cursor == records[i].push_cursor);
	}

	// Wrong revision / GPU must be ignored, never crash.
	Check(!LoadFile(path, "rev2", "gpu1", loaded));
	Check(!LoadFile(path, "rev1", "gpu2", loaded));
	// Missing file must be ignored.
	Check(!LoadFile(root / "nope.bin", "rev1", "gpu1", loaded));

	// Corrupted payload must be rejected.
	{
		std::FILE* file = nullptr;
#if defined(_WIN32)
		fopen_s(&file, path.string().c_str(), "r+b");
#else
		file = std::fopen(path.string().c_str(), "r+b");
#endif
		Check(file != nullptr);
		std::fseek(file, 20, SEEK_SET);
		std::fputc(0xFF, file);
		std::fclose(file);
	}
	Check(!LoadFile(path, "rev1", "gpu1", loaded));

	// Out-of-bounds records are never written.
	{
		std::vector<Record> bad;
		Record              huge = MakeRecord(kInputVertex, 0x4444u);
		huge.code_words          = 0;
		bad.push_back(huge);
		Check(SaveFile(path, "rev1", "gpu1", bad));
		Check(LoadFile(path, "rev1", "gpu1", loaded));
		Check(loaded.empty());
	}

	// Phase 2: specialization and compiled-info round-trip through blobs.
	{
		using IRSpec = Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization;
		using IRInfo = Libs::Graphics::ShaderRecompiler::IR::CompiledShaderInfo;
		IRSpec spec;
		spec.buffers.push_back(IRSpec::Buffer {
		    16u, Libs::Graphics::Prospero::BufferFormat::kInvalid, 0xfacu, true});
		spec.images.push_back(IRSpec::Image {
		    Libs::Graphics::Prospero::TextureNumericClass::Unsupported,
		    Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Unknown, 4u,
		    Libs::Graphics::Prospero::BufferFormat::kInvalid, 0xfacu, 7u, 8u, 9u, true, false});
		spec.enable_lod_stats = true;
		std::vector<uint8_t> spec_blob;
		EncodeSpecialization(spec, spec_blob);
		IRSpec spec_back;
		Check(DecodeSpecialization(spec_blob.data(), spec_blob.size(), spec_back));
		Check(spec_back == spec);
		Check(!DecodeSpecialization(spec_blob.data(), spec_blob.size() - 1, spec_back));

		IRInfo info;
		info.stage            = Libs::Graphics::ShaderType::Compute;
		info.shader_hash      = 0xabcdu;
		info.wave_size        = 32;
		info.param_export_mask = 3;
		info.bda_read_plan.complete = true;
		info.info.buffers.push_back(
		    Libs::Graphics::ShaderRecompiler::IR::BufferResource {});
		info.info.images.push_back(Libs::Graphics::ShaderRecompiler::IR::ImageResource {});
		info.info.samplers.push_back(
		    Libs::Graphics::ShaderRecompiler::IR::SamplerResource {});
		info.info.sampled_pairs.push_back(
		    Libs::Graphics::ShaderRecompiler::IR::SampledResourcePair {1u, 2u, 3u});
		info.info.inputs.push_back(Libs::Graphics::ShaderRecompiler::IR::StageInput {});
		info.info.outputs.push_back(Libs::Graphics::ShaderRecompiler::IR::StageOutput {});
		info.info.vertex_fetch_components[0] = 7;
		info.bindings.push_data_start_dword  = 6;
		info.bindings.user_data_registers    = {10u, 11u};
		Libs::Graphics::ShaderRecompiler::IR::DescriptorBinding binding;
		binding.resources = {1u, 2u};
		info.bindings.descriptors.push_back(binding);
		std::vector<uint8_t> info_blob;
		EncodeCompiledInfo(info, info_blob);
		IRInfo info_back;
		Check(DecodeCompiledInfo(info_blob.data(), info_blob.size(), info_back));
		// No operator== on the full info: re-encode must be byte-identical.
		std::vector<uint8_t> info_blob2;
		EncodeCompiledInfo(info_back, info_blob2);
		Check(info_blob == info_blob2);
		Check(!DecodeCompiledInfo(info_blob.data(), info_blob.size() - 1, info_back));

		// Record carrying a variant survives the file, and merges on re-add.
		Record with_variant = MakeRecord(kInputCompute, 0x5555u);
		with_variant.variants.push_back(
		    SpvVariant {.spec = spec_blob, .info = info_blob, .spirv = {1u, 2u, 3u, 4u}});
		Recorder recorder;
		recorder.Configure(path, "rev1", "gpu1");
		recorder.Add(std::move(with_variant));
		Record second = MakeRecord(kInputCompute, 0x5555u);
		second.variants.push_back(
		    SpvVariant {.spec = {9u, 9u}, .info = info_blob, .spirv = {5u}});
		recorder.Add(std::move(second));
		Check(recorder.Size() == 1);
		recorder.Flush();
		Check(LoadFile(path, "rev1", "gpu1", loaded));
		Check(loaded.size() == 1);
		Check(loaded[0].variants.size() == 2);
		Check(loaded[0].variants[0].spirv == std::vector<uint32_t>({1u, 2u, 3u, 4u}));
		Check(loaded[0].variants[0].spec == spec_blob);
		Check(loaded[0].variants[0].info == info_blob);

		// Seed + FetchCopy: the warmup loop's view of the recorder.
		Recorder seeded;
		seeded.Configure(path, "rev1", "gpu1");
		Check(seeded.Seed(std::move(loaded)) == 1);
		Record fetched;
		Check(seeded.FetchCopy(0, fetched));
		Check(fetched.variants.size() == 2);
		Check(!seeded.FetchCopy(1, fetched));
		// A live novel variant merges into the seeded record.
		Record novel = MakeRecord(kInputCompute, 0x5555u);
		novel.variants.push_back(SpvVariant {.spec = {7u}, .info = info_blob, .spirv = {6u}});
		seeded.Add(std::move(novel));
		Check(seeded.Size() == 1);
		Check(seeded.FetchCopy(0, fetched));
		Check(fetched.variants.size() == 3);
	}

	std::filesystem::remove_all(root);
}
