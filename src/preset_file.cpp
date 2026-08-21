// Copyright (c) 2026 Martin Erzberger. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "preset_file.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <filesystem>
#include <set>

namespace pc1500preset {

namespace {

std::string resolveRelative(const std::string& p, const std::filesystem::path& baseDir) {
  if (p.empty()) return p;
  std::filesystem::path path(p);
  if (path.is_absolute()) return p;
  return (baseDir / path).string();
}

// Accepts "0xXXXX"/"0XXXXX", "&XXXX" (BASIC's own hex prefix, still accepted
// here for leniency even though docs/preset_file_format.md recommends "0x"
// for structural fields), or bare hex digits.
bool parseHex16(const std::string& token, uint16_t* out) {
  std::string t = token;
  if (!t.empty() && t[0] == '&') t = t.substr(1);
  if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
  if (t.empty()) return false;
  for (char c : t) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  unsigned long v = std::stoul(t, nullptr, 16);
  if (v > 0xFFFF) return false;
  *out = static_cast<uint16_t>(v);
  return true;
}

// Accepts a decimal byte count, optionally suffixed with 'k'/'K' (*1024).
bool parseSize(const std::string& token, size_t* out) {
  if (token.empty()) return false;
  std::string t = token;
  size_t multiplier = 1;
  if (t.back() == 'k' || t.back() == 'K') {
    multiplier = 1024;
    t.pop_back();
  }
  if (t.empty()) return false;
  for (char c : t) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  *out = static_cast<size_t>(std::stoul(t)) * multiplier;
  return true;
}

const std::set<std::string>& topLevelKeywords() {
  static const std::set<std::string> kKeywords = {
      "model",       "firmware",        "memory-expansion", "rom-modules",
      "program",     "pre-load-keys",   "post-load-keys",
  };
  return kKeywords;
}

int lineOf(const YAML::Node& n) { return n.Mark().line + 1; }

}  // namespace

bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error) {
  *out = PresetFile{};

  auto failAtLine = [&](int line, const std::string& msg) {
    if (error) *error = path + ":" + std::to_string(line) + ": " + msg;
    return false;
  };

  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::BadFile&) {
    if (error) *error = "could not open '" + path + "'";
    return false;
  } catch (const YAML::Exception& e) {
    return failAtLine(e.mark.line + 1, e.msg);
  }

  std::filesystem::path baseDir = std::filesystem::path(path).parent_path();
  bool loadAddressSet = false;

  auto failAt = [&](const YAML::Node& n, const std::string& msg) { return failAtLine(lineOf(n), msg); };
  auto failFile = [&](const std::string& msg) {
    if (error) *error = path + ": " + msg;
    return false;
  };

  try {
    if (!root.IsMap()) {
      return failFile("top level must be a mapping of fields -- see docs/preset_file_format.md");
    }

    for (const auto& entry : root) {
      std::string key = entry.first.as<std::string>();
      if (!topLevelKeywords().count(key)) {
        return failAt(entry.first, "unrecognized field '" + key + "'");
      }
    }

    if (YAML::Node n = root["model"]) {
      std::string v = n.as<std::string>();
      if (v != "PC-1500" && v != "PC-1500A") {
        return failAt(n, "model must be 'PC-1500' or 'PC-1500A'");
      }
      out->model = v;
    }

    if (YAML::Node n = root["firmware"]) {
      out->firmwarePath = resolveRelative(n.as<std::string>(), baseDir);
    }

    if (YAML::Node n = root["memory-expansion"]) {
      if (!n.IsSequence()) return failAt(n, "'memory-expansion' must be a list");
      for (const YAML::Node& item : n) {
        if (!item.IsMap()) {
          return failAt(item, "each memory-expansion entry must be a mapping with 'address' and 'size'");
        }
        YAML::Node addrNode = item["address"];
        YAML::Node sizeNode = item["size"];
        YAML::Node moduleNode = item["module"];
        if (!addrNode || (!sizeNode && !moduleNode)) {
          return failAt(item, "memory-expansion entry needs 'address' and either 'size' or 'module'");
        }
        if (sizeNode && moduleNode) {
          return failAt(item, "memory-expansion entry cannot have both 'size' and 'module'");
        }
        uint16_t window = 0;
        std::string addrStr = addrNode.as<std::string>();
        if (!parseHex16(addrStr, &window)) {
          return failAt(addrNode, "invalid memory-expansion address '" + addrStr + "'");
        }
        if (moduleNode) {
          std::string moduleStr = moduleNode.as<std::string>();
          if (moduleStr != "ce163" && moduleStr != "ce168n" && moduleStr != "cemax" &&
              moduleStr != "cemaxa" && moduleStr != "ce155") {
            return failAt(moduleNode, "unrecognized memory-expansion module '" + moduleStr + "'");
          }
          if (window != 0x0000) {
            return failAt(addrNode, "'module: " + moduleStr + "' requires address 0x0000");
          }
          if (moduleStr == "ce163") {
            if (item["bank-content"]) {
              return failAt(item, "'bank-content' is only valid with 'module: ce168n'");
            }
            out->ce163Enabled = true;
            continue;
          }
          if (moduleStr == "cemax" || moduleStr == "cemaxa") {
            if (item["bank-content"]) {
              return failAt(item, "'bank-content' is only valid with 'module: ce168n'");
            }
            // Both extension-RAM windows at their own max size, model-
            // matched: 'cemax' is the PC-1500's 10K expansion window,
            // 'cemaxa' the PC-1500A's 6K one (pc1500emu's own
            // Bus::extRamExtWindowMaxSize() for each variant). Sending
            // the wrong one's max on the other model would silently spill
            // configured "RAM" past the real 6/10K window boundary into
            // fixed system RAM at 0x7000+ (pc1500emu's setextram doesn't
            // clamp), so this is checked here at parse time rather than
            // left to fail confusingly (or not at all) on-device.
            bool wantsA = (moduleStr == "cemaxa");
            if (out->model.empty()) {
              return failAt(item,
                             "'module: " + moduleStr + "' requires the top-level 'model' field");
            }
            bool isA = (out->model == "PC-1500A");
            if (wantsA != isA) {
              return failAt(item, "'module: " + moduleStr + "' requires model: " +
                                       (wantsA ? "PC-1500A" : "PC-1500") + " (this file has " +
                                       out->model + ")");
            }
            out->extRam0000Bytes = 16384;
            out->extRam4800Bytes = wantsA ? 6144 : 10240;
            continue;
          }
          if (moduleStr == "ce155") {
            if (item["bank-content"]) {
              return failAt(item, "'bank-content' is only valid with 'module: ce168n'");
            }
            out->ce155Enabled = true;
            continue;
          }
          YAML::Node banksNode = item["banks"];
          YAML::Node firstRoBankNode = item["first-read-only-bank"];
          if (!banksNode || !firstRoBankNode) {
            return failAt(item, "'module: ce168n' requires 'banks' and 'first-read-only-bank'");
          }
          int banks = 0;
          try {
            banks = std::stoi(banksNode.as<std::string>());
          } catch (...) {
            return failAt(banksNode, "invalid 'banks' value");
          }
          if (banks < 1) return failAt(banksNode, "'banks' must be >= 1");
          int firstRoBank = 0;
          try {
            firstRoBank = std::stoi(firstRoBankNode.as<std::string>());
          } catch (...) {
            return failAt(firstRoBankNode, "invalid 'first-read-only-bank' value");
          }
          if (firstRoBank < 0) {
            return failAt(firstRoBankNode, "'first-read-only-bank' must be >= 0");
          }
          out->ce168nBanks = banks;
          out->ce168nFirstRoBank = firstRoBank;
          if (YAML::Node contentNode = item["bank-content"]) {
            if (!contentNode.IsSequence()) {
              return failAt(contentNode, "'bank-content' must be a list");
            }
            for (const YAML::Node& c : contentNode) {
              if (!c.IsMap()) return failAt(c, "each bank-content entry must be a mapping");
              YAML::Node bankNode = c["bank"];
              YAML::Node pathNode = c["path"];
              if (!bankNode || !pathNode) {
                return failAt(c, "bank-content entry needs 'bank' and 'path'");
              }
              Ce168nBankContent bc;
              try {
                bc.bank = std::stoi(bankNode.as<std::string>());
              } catch (...) {
                return failAt(bankNode, "invalid 'bank' value");
              }
              if (bc.bank < 0 || bc.bank >= banks) {
                return failAt(bankNode, "'bank' must be < banks (" + std::to_string(banks) + ")");
              }
              bc.path = resolveRelative(pathNode.as<std::string>(), baseDir);
              out->ce168nBankContent.push_back(bc);
            }
          }
          continue;
        }
        if (item["bank-content"]) {
          return failAt(item, "'bank-content' is only valid with 'module: ce168n'");
        }
        size_t bytes = 0;
        std::string sizeStr = sizeNode.as<std::string>();
        if (!parseSize(sizeStr, &bytes)) {
          return failAt(sizeNode, "invalid memory-expansion size '" + sizeStr + "'");
        }
        if (window == 0x0000) {
          out->extRam0000Bytes = bytes;
        } else if (window == 0x4800) {
          out->extRam4800Bytes = bytes;
        } else {
          return failAt(addrNode, "memory-expansion address must be 0x0000 or 0x4800");
        }
      }
    }

    if (YAML::Node n = root["rom-modules"]) {
      if (!n.IsSequence()) return failAt(n, "'rom-modules' must be a list");
      for (const YAML::Node& item : n) {
        if (!item.IsMap()) return failAt(item, "each rom-modules entry must be a mapping");
        YAML::Node slotNode = item["slot"];
        YAML::Node addrNode = item["address"];
        YAML::Node pathNode = item["path"];
        if (!slotNode || !addrNode || !pathNode) {
          return failAt(item, "rom-modules entry needs 'slot', 'address', and 'path'");
        }
        RomModuleAttachment m;
        std::string slotStr = slotNode.as<std::string>();
        bool slotOk = (slotStr == "auto");
        if (slotOk) {
          m.slot = 0;
        } else {
          try {
            m.slot = std::stoi(slotStr) - 1;
            slotOk = (m.slot >= 0 && m.slot <= 3);
          } catch (...) {
            slotOk = false;
          }
        }
        if (!slotOk) return failAt(slotNode, "slot must be 1-4 or 'auto'");
        std::string addrStr = addrNode.as<std::string>();
        if (!parseHex16(addrStr, &m.base)) {
          return failAt(addrNode, "invalid rom-modules address '" + addrStr + "'");
        }
        if (YAML::Node reqNode = item["require-pv"]) m.requirePv = reqNode.as<bool>();
        if (YAML::Node useNode = item["use-pu-bank"]) m.usePuBank = useNode.as<bool>();
        m.path = resolveRelative(pathNode.as<std::string>(), baseDir);
        out->romModules.push_back(m);
      }
    }

    if (YAML::Node n = root["program"]) {
      if (!n.IsMap()) return failAt(n, "'program' must be a mapping");
      YAML::Node pathNode = n["path"];
      YAML::Node textNode = n["text"];
      if (!pathNode && !textNode) return failAt(n, "'program' needs a 'path' or 'text'");
      if (pathNode && textNode) return failAt(n, "'program' cannot have both 'path' and 'text'");
      out->hasProgram = true;
      if (pathNode) {
        out->programPath = resolveRelative(pathNode.as<std::string>(), baseDir);
      } else {
        out->programText = textNode.as<std::string>();
      }
      if (YAML::Node fmtNode = n["format"]) {
        std::string fmt = fmtNode.as<std::string>();
        if (fmt == "binary") {
          out->programFormat = ProgramFormat::kBinary;
        } else if (fmt == "basic-tokenized") {
          out->programFormat = ProgramFormat::kBasicTokenized;
        } else if (fmt == "basic-text") {
          out->programFormat = ProgramFormat::kBasicText;
        } else {
          return failAt(fmtNode, "format must be 'binary', 'basic-tokenized', or 'basic-text'");
        }
      }
      if (out->programText && out->programFormat != ProgramFormat::kBasicText) {
        return failAt(n, "'program.text' is only valid with format 'basic-text'");
      }
      if (YAML::Node addrNode = n["address"]) {
        std::string addrStr = addrNode.as<std::string>();
        if (!parseHex16(addrStr, &out->loadAddress)) {
          return failAt(addrNode, "invalid program address '" + addrStr + "'");
        }
        loadAddressSet = true;
      }
    }

    auto parseScript = [&](const YAML::Node& n, std::vector<ScriptLine>* dest) -> bool {
      if (!n.IsSequence()) return failAt(n, "must be a list of steps");
      for (const YAML::Node& item : n) {
        if (!item.IsMap() || item.size() != 1) {
          return failAt(item, "each step must be a single-key mapping, e.g. '- key: cl'");
        }
        auto it = item.begin();
        std::string verb = it->first.as<std::string>();
        YAML::Node valueNode = it->second;
        ScriptLine script;
        if (verb == "key" || verb == "type") {
          script.kind = (verb == "key") ? ScriptLineKind::kKeyPress : ScriptLineKind::kType;
          script.text = valueNode.as<std::string>();
        } else if (verb == "check") {
          script.kind = ScriptLineKind::kCheck;
          script.text = valueNode.as<std::string>();
          // `check` works by encoding the expected text into the PC-1500's
          // own dot-matrix glyphs and searching the live display for a
          // match (see main.cpp's displayContainsGlyph()) -- there's no
          // decoder the other way, so only expected values with a known
          // glyph encoding are accepted. Only "0" has one so far; extend
          // this alongside main.cpp's glyph table together, never one
          // without the other.
          if (script.text != "0") {
            return failAt(valueNode, "'check " + script.text +
                                          "' is not supported yet -- only 'check 0' is (see "
                                          "preset_file.cpp's comment on the 'check' branch)");
          }
        } else if (verb == "wait") {
          script.kind = ScriptLineKind::kWait;
          try {
            script.waitSeconds = valueNode.as<double>();
          } catch (const YAML::Exception&) {
            return failAt(valueNode, "invalid 'wait' duration '" + valueNode.as<std::string>() + "'");
          }
        } else {
          return failAt(item, "expected 'key', 'type', 'wait', or 'check', got '" + verb + "'");
        }
        if (script.kind != ScriptLineKind::kWait && script.text.empty()) {
          return failAt(valueNode, "missing value for '" + verb + "'");
        }
        dest->push_back(script);
      }
      return true;
    };

    if (YAML::Node n = root["pre-load-keys"]) {
      if (!parseScript(n, &out->preLoadKeys)) return false;
    }
    if (YAML::Node n = root["post-load-keys"]) {
      if (!parseScript(n, &out->postLoadKeys)) return false;
    }
  } catch (const YAML::Exception& e) {
    return failAtLine(e.mark.line + 1, e.msg);
  }

  if (out->model.empty()) {
    return failFile("missing required 'model' field");
  }
  if (out->firmwarePath.empty()) {
    out->firmwarePath = resolveRelative("roms/PC-1500_A04.ROM", baseDir);
  }
  if (out->hasProgram && out->programFormat == ProgramFormat::kBinary && !loadAddressSet) {
    return failFile("'program.address' is required when program.format is 'binary'");
  }
  return true;
}

}  // namespace pc1500preset
