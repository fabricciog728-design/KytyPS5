#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWARMUP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWARMUP_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct ResourceSpecialization;
struct CompiledShaderInfo;
} // namespace Libs::Graphics::ShaderRecompiler::IR

namespace Libs::Graphics::ShaderWarmup {

// Disk-backed shader warmup cache ("record-then-replay").
//
// During play, every shader that misses ProgramCache (i.e. pays a full
// TranslateProgram + CompileProgram hitch) is recorded to
// `_ShaderCache/<TITLE_ID>.bin`. On the next boot, a background thread
// replays the records through the same ProgramCache::Get path, so the
// expensive work moves from gameplay frames to loading time.
//
// Replay correctness notes (see pipelineCache.cpp):
// - Translation is a pure function of (code bytes, stage, wave size,
//   user-data layout and the plain-data input snapshot). Guest addresses
//   inside the snapshot are never read by the translate phase.
// - Materialization reads live guest memory through the same callbacks as
//   real draws. Warmup swaps the raw-memory reader for a Try-only variant
//   and treats a failed materialization as "not ready yet" instead of
//   aborting, so stale records can never crash boot.

// Which InputInfo template the record belongs to. Vertex and mesh stages
// share ShaderVertexInputInfo.
inline constexpr uint8_t kInputVertex  = 0;
inline constexpr uint8_t kInputPixel   = 1;
inline constexpr uint8_t kInputCompute = 2;

// Sanity bounds for the on-disk format. Records beyond these are rejected
// both when recording and when loading.
inline constexpr uint32_t kMaxRecords       = 16384;
inline constexpr uint32_t kMaxCodeWords     = 256u * 1024u; // 1 MiB, mirrors HashShaderCode
inline constexpr uint32_t kMaxUserDataWords = 256;
inline constexpr uint32_t kMaxBackCodeWords = 64u * 1024u;
inline constexpr uint32_t kMaxInputBytes    = 8192;
inline constexpr uint32_t kFlushEveryRecords = 64;
inline constexpr uint64_t kMaxFileBytes      = 256ull * 1024ull * 1024ull;
// Phase 2 (persisted compilations): bounds per stored permutation.
inline constexpr uint32_t kMaxVariantsPerRecord = 16;
inline constexpr uint32_t kMaxSpirvWords        = 256u * 1024u; // 1 MiB of SPIR-V
inline constexpr uint32_t kMaxSpecBytes         = 64u * 1024u;
inline constexpr uint32_t kMaxInfoBytes         = 1024u * 1024u;

// One compiled permutation: everything a real draw needs besides the module
// handle (recreated from spirv at replay). All blobs are plain-data
// serializations, see Encode*/Decode* below.
struct SpvVariant {
	std::vector<uint8_t>  spec;  // encoded ResourceSpecialization
	std::vector<uint8_t>  info;  // encoded CompiledShaderInfo
	std::vector<uint32_t> spirv; // SPIR-V words
};

struct Record {
	uint8_t  stage      = 0; // ShaderType as u8
	uint64_t hash       = 0; // ShaderParams::hash (declared or XXH3 of code)
	uint64_t code_addr  = 0; // guest address, re-read live and hash-verified on replay
	uint32_t code_words = 0;

	std::vector<uint32_t> user_data; // recorded values (needed for specialization)
	std::vector<uint32_t> back_code; // fused back-half bytes (usually empty)

	uint32_t wave_size      = 0;
	uint32_t user_data_base = 0;
	uint32_t scratch_dwords = 0;
	uint32_t push_cursor    = 0; // entry value, before AdvancePushData

	uint8_t input_kind = kInputVertex;
	// Raw input struct bytes split around the `stage` output member, so replay
	// never memcpys over a live std::vector: head = [0, stage), tail = rest.
	// head.size() is the stage offset; head.size() + tail.size() + stage size
	// must equal sizeof the input struct for input_kind.
	uint32_t             input_size = 0;
	std::vector<uint8_t> input_head;
	std::vector<uint8_t> input_tail;
	// Phase 2: compiled permutations recorded alongside the shader. Empty for
	// v1 files; replay then falls back to a full Get recompile.
	std::vector<SpvVariant> variants;
};

// Stable dedupe key (FNV-1a over every field that affects compilation,
// excluding stored variants: new permutations merge into the record).
[[nodiscard]] uint64_t RecordKey(const Record& record) noexcept;

// Plain-data serializers for phase 2. Encoding is deterministic (fixed field
// order, enums as u32, bools as u8), so byte equality implies struct equality.
void EncodeSpecialization(const ShaderRecompiler::IR::ResourceSpecialization& spec,
                          std::vector<uint8_t>& out);
[[nodiscard]] bool DecodeSpecialization(const uint8_t* data, size_t size,
                                        ShaderRecompiler::IR::ResourceSpecialization& spec);
void EncodeCompiledInfo(const ShaderRecompiler::IR::CompiledShaderInfo& info,
                        std::vector<uint8_t>& out);
[[nodiscard]] bool DecodeCompiledInfo(const uint8_t* data, size_t size,
                                      ShaderRecompiler::IR::CompiledShaderInfo& info);

// Atomic file update via temp + rename. Returns false (no crash, no partial
// file) on any IO error.
[[nodiscard]] bool SaveFile(const std::filesystem::path& path, const std::string& git_revision,
                            const std::string& gpu_signature, const std::vector<Record>& records);
// Returns false when the file is missing, corrupt, or was written by another
// emulator revision / GPU. Callers must treat false as "no warmup data".
[[nodiscard]] bool LoadFile(const std::filesystem::path& path, const std::string& git_revision,
                            const std::string& gpu_signature, std::vector<Record>& records_out);

// Session-side accumulator. Add() validates bounds and dedupes; the file is
// rewritten every kFlushEveryRecords additions and on Flush().
class Recorder {
public:
	Recorder()                                 = default;
	Recorder(const Recorder&)                  = delete;
	Recorder& operator=(const Recorder&)       = delete;

void Configure(std::filesystem::path path, std::string git_revision,
	               std::string gpu_signature);
	void Add(Record record);
	void Flush();
	[[nodiscard]] size_t Size() const { return records_.size(); }
	// Moves disk-loaded records in, merging with anything recorded meanwhile.
	// Returns Size() afterwards (the warmup loop bound). Not thread-safe; the
	// caller must serialize with Add/Flush (PipelineCache uses m_mutex).
	size_t Seed(std::vector<Record> loaded);
	// Copies one record out for processing. False when out of range. The copy
	// keeps retry passes cheap and race-free; same locking as above.
	[[nodiscard]] bool FetchCopy(size_t index, Record& out) const;

private:
	// Merges a variant into an existing record (matched by spec bytes, which
	// are deterministic). Returns true when the record changed.
	bool MergeVariant(Record& existing, SpvVariant variant);
	// Merges all variants of src into the record at pos. Returns true when it
	// changed.
	bool MergeInto(size_t pos, Record& src);

	std::filesystem::path                path_;
	std::string                          git_revision_;
	std::string                          gpu_signature_;
	std::vector<Record>                  records_;
	std::unordered_map<uint64_t, size_t> index_;
	size_t                               dirty_      = 0;
	bool                                 configured_ = false;
};

} // namespace Libs::Graphics::ShaderWarmup

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWARMUP_H_
