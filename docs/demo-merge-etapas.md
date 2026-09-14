# Demo-merge PR500–PR599 — Roadmap de etapas

Branch: `demo-merge-pr500-pr599` · Base: `fc69b20` (merge PR500 + PR599 verbatim).
Regra: 1 etapa por commit, port fiel da `submission` verde, CI precisa ficar verde.

## Etapas

| # | Status | Commit | Escopo (arquivos) | Teste |
|---|--------|--------|-------------------|-------|
| 1 | ✅ | `8ddd6a7` | Estabilidade base GPU: `masterSemaphore.cpp` (bounded 1s + Refresh retry), `samplerCache.{h,cpp}` (reserve 128 + GC 256/8192), `bufferCache.cpp` (reserve 8 + barreira estreita ao span) | CI existente |
| 2 | ✅ | `c2b7224` | Draw-index batching: `render.h` (`kMaxDrawIndexRunDraws=128`), `renderDraw.h`, `graphicsRun.cpp` (sem trava TriList), `renderDraw.cpp` (density check + topologia real) | CI existente + boot 3D |
| 3 | ✅ | `d7a5162` | Warmup record-then-replay v1+v2: `shaderWarmup.{h,cpp}` (novos), `pipelineCache.{h,cpp}`, `ShaderWarmupTests.cpp` (novo), CMake só `shader_warmup_tests` | `ctest -R "^shader_warmup$"` + boot 2x |
| 4 | ✅ | `8a13191` | Recompiler hot-path: `ShaderRecompiler.{cpp,h}` (thread_local, rebuild-vs-copy, passes enxutos, `dump_ir=false`), `shader.cpp` (shared_mutex + XXH3 direto >64K), `SpirvBuilder.{h,cpp}` (reserve + unordered dedup), `SpirvEmitter.cpp` (validate só NDEBUG) | `shader_cfg`, `shader_recompiler_compute`, `resource_*` |
| 5 | ✅ | `66d5b02` | LOD single-bit: `ResourceMaterialization.{h,cpp}` (`enable_lod_stats`), `ResourceMaterializationTests.cpp`, `ShaderRecompilerComputeTests.cpp`, CMake `shader_lod_emission` | `shader_lod_feedback`, `shader_lod_emission` |
| CI | ✅ | `5f918ad` | `.github/workflows/build.yml`: push também em `demo-merge-pr500-pr599` | Actions verde |
| 6 | ✅ | (este commit) | Áudio/vídeo HLE: `audio.cpp` (SDL_CVT cached, fast-path, AudioTrace), `libAudio2.cpp`, `ajm.cpp`, `videoDec2Decoder.cpp` (tail zeroing), `network.cpp`, `emulatorConfig` + `main.cpp` (`--audio-trace`) | `audio_out2_port`, `emulator_audio_trace_cli` |
| 7 | ✅ | (este commit) | Build/launcher/config: Thin LTO release, `WIN32_LEAN_AND_MEAN`, `alignment.h` removido (sem uso), launcher `configuration*`, `libJson2` sem `JsonValueClearMethod`, README | CI launcher |
| 8a | ✅ | `7e51a27` | ISA: `S_MUL_HI_I32` (SOP2 0x36, re-aplica #576 derrubado pelo alinhamento) | `shader_cfg` |
| 8b | ✅ | `afecaf9` | ISA: `V_FRACT_F16` (VOP1 0x5f) + `V_CMPX_LT_U16` (VOPC 0xb9, re-aplica #582) + filtros `--fract-f16-only`/`--cmpx-lt-u16-only` | `shader_cfg`, `shader_recompiler_compute` |
| 8c | ✅ | (este commit) | ISA inédito: `S_CMP_LT_U64` (SOPC 0x16, ordem GT/GE/LT/LE dos blocos I32/U32 + `ULessThan64` existente); teste standalone `TestNewShaderRecompilerSopcLtU64`; corrige 2 expectativas 8a para nomes IR atuais | `shader_cfg` |
| 9 | ⬜ | — | SBO/dynamic-state/present parkados pelo revert `f62a4c8` — **ausentes do tip da submission (vermelhos); re-lançar 1 peça por vez com sinal de CI próprio, fora deste commit** | CI por peça |
| 10 | ⬜ | — | Observabilidade restante: hash naming, dump SPIR-V, `spirv-val` no CI (LTO release feito neste commit) | CI |

## Dependência crítica (aprendida no CI)

`shaderWarmup.cpp:Encode/DecodeSpecialization` (`:591,:623`) usa
`ResourceSpecialization::enable_lod_stats` (`ResourceMaterialization.h:38`).
Warmup (etapa 3) sem esse campo (etapa 5) = erro de compilação em todos os
TUs full-emulator nas 3 plataformas. Serializer + struct são atômicos —
nunca fatiar um sem o outro.

## CI (Actions) — histórico

| Run | Commit | Evento | Resultado | Causa |
|-----|--------|--------|-----------|-------|
| 2 | `c2b7224` etapa 2 | dispatch | ✅ success | sem warmup, sem dependência nova |
| 3 | `d7a5162` etapa 3 | dispatch | ❌ failure (Build nas 3 OS, Configure ok) | warmup referencia `enable_lod_stats` inexistente |
| 4 | `5f918ad` CI | push | ❌ failure (Build nas 3 OS, Configure ok) | mesma causa (etapa 4 não traz o campo) |
| 5 | `66d5b02` etapa 5 | push | ✅ success (Linux+macOS+Windows build+test+install) | campo adicionado; `Release` skipped por ser fork (condição `KytyPS5/KytyPS5`) |

Comandos: `ctest -R "^(audio_out2_port|ime_dialog|virtual_memory_allocation|page_manager|memory_tracker|shader_warmup|shader_lod_feedback|shader_lod_emission)$"` + boot 2x do mesmo jogo (1º grava `_ShaderCache`, 2º replay em background).

## Nota: expectativas obsoletas em `shaderCfgTests.cpp`

`decoded_dump` usa `magic_enum` (opcodes MAIÚSCULOS) e `ir_dump` usa
`ValueOpcodeName` (nomes atuais do `.inc`, ex `SMulHi`, `INotEqual64`).
Checks com nomes da era pré-`ffa69d4` (ex `s_add_u32`, `UMulHighU32`,
`CompareNeU64`) e os testes mortos `ScalarVectorAlu`/`ScalarBitfieldAlu`
(nunca chamados no `main`) são Faxina futura — precisa de ambiente com build
para verificar linha a linha. Testes novos usam só formato atual.
