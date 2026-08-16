// Copyright (c) 2026 Martin Erzberger. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Parser for the plain-text, human-editable, diffable ".pc1500" file
// format (see docs/preset_file_format.md for the full grammar). This parser
// is deliberately standalone -- it has no dependency on the emulator's own
// Bus/CPU code, unlike src/hoststate/preset_file.h on the feature/preset
// branch this was forked from. That version applied a preset by calling
// straight into a live Bus/CPU instance running in the same process; this
// tool instead drives an unmodified, separately-launched pc1500emu process
// over its existing FIFO/pipe scripting interface (see main.cpp), so the
// emulator itself never needs to change to support presets, and stays free
// to diverge from this fork's own code as upstream evolves.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pc1500preset {

enum class ProgramFormat {
  kBinary,          // flat byte range loaded at loadAddress (loadbinary)
  kBasicTokenized,  // already-tokenized BASIC bytes (loadbasic)
  kBasicText,       // plain-text BASIC listing, typed via loadbasictext
};

enum class ScriptLineKind {
  kKeyPress,  // `key <name>`
  kType,      // `type <text>`
  kWait,      // `wait <seconds>`
  kCheck,     // `check <expected text>`
};

// One line from a pre-load-keys/post-load-keys block, in file order.
struct ScriptLine {
  ScriptLineKind kind = ScriptLineKind::kKeyPress;
  std::string text;      // key name, typed text, or (kCheck) expected display text
  double waitSeconds = 0;  // only meaningful for kWait
};

struct RomModuleAttachment {
  int slot = 0;  // 0-3, matching the FIFO loadrommodule/2/3/4 commands
  uint16_t base = 0xA000;
  bool requirePv = false;
  bool usePuBank = false;
  std::string path;  // resolved relative to the preset file's own directory
};

struct PresetFile {
  // Purely descriptive -- see docs/preset_file_format.md.
  std::string model;

  // Resolved relative to the preset file's own directory. Defaults to
  // roms/PC-1500_A04.ROM (also resolved relative to the preset file) when
  // the file doesn't specify one.
  std::string firmwarePath;

  std::optional<size_t> extRam0000Bytes;
  std::optional<size_t> extRam4800Bytes;

  std::vector<RomModuleAttachment> romModules;

  bool hasProgram = false;
  // Exactly one of these is set when hasProgram is true: `path` (resolved
  // relative to the preset file's own directory) loads an external file;
  // `text` is an inline BASIC listing (only valid with kBasicText) written
  // to a temp file at apply time, since the FIFO loadbasictext command
  // only takes a path -- see main.cpp's applyPreset().
  std::string programPath;
  std::optional<std::string> programText;
  ProgramFormat programFormat = ProgramFormat::kBinary;
  uint16_t loadAddress = 0;  // required (and meaningful) only for kBinary

  std::vector<ScriptLine> preLoadKeys;
  std::vector<ScriptLine> postLoadKeys;
};

// Parses `path`. Returns false with *error set (naming the offending line
// number) on any malformed input -- a preset file is applied all-or-nothing,
// never partially, so parsing fails before anything touches the machine.
bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error);

}  // namespace pc1500preset
