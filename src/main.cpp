// Copyright (c) 2026 Martin Erzberger. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// pc1500preset: applies a .pc1500 file (docs/preset_file_format.md)
// to a completely unmodified pc1500emu binary (an independent sibling
// checkout at ../pc1500emu -- see this project's own top-level README),
// driving it entirely through its own existing FIFO/pipe scripting
// interface (../pc1500emu/README.md's "Scriptable command interface"
// section) rather than linking against or patching the emulator's own
// source. See preset_file.h's top comment for the rationale.
//
// POSIX only for now (uses a real FIFO, fork/exec, and signals) -- the
// emulator's own Windows story for this same interface is a named pipe
// plus tools/send-command.ps1; a Windows port of this tool would follow
// that same pattern, but isn't implemented yet.
#include <spawn.h>
#include <sys/wait.h>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "preset_file.h"

extern char** environ;

namespace {

using pc1500preset::PresetFile;
using pc1500preset::ProgramFormat;
using pc1500preset::ScriptLine;
using pc1500preset::ScriptLineKind;

#if defined(__APPLE__) || defined(__linux__)
constexpr const char* kCommandFifoPath = "/tmp/pc1500emu.cmd";
constexpr const char* kResponsePath = "/tmp/pc1500emu.response";
#endif

// Real-time delay after a `key` script line (which only queues into the
// live emulator's frame-driven tap/idle sequence -- see ../pc1500emu's
// own main.cpp, symbolActionQueue -- rather than executing synchronously)
// and, for uniformity, after every script line generally. A
// prototype-grade stand-in for actually watching the display to detect
// when a statement has finished -- see docs/preset_file_format.md's note
// on the same tradeoff.
constexpr auto kPostStatementSettle = std::chrono::milliseconds(500);

// Real-time delay after `reset`, before sending any scripted keystrokes:
// the ROM's own power-on RAM-check/boot sequence doesn't start polling the
// keyboard until it settles into its post-boot idle loop (confirmed
// empirically, via cycle-accurate stepping, by an earlier in-process
// prototype of this same preset format -- see docs/preset_file_format.md).
// This tool can't step cycles directly since it never touches the
// emulator's own CPU/bus, only its FIFO interface, so it waits a
// comfortably generous real-time margin instead of a precise cycle count.
constexpr auto kBootSettle = std::chrono::milliseconds(1500);

constexpr auto kFifoReadyPollInterval = std::chrono::milliseconds(100);
constexpr auto kFifoReadyTimeout = std::chrono::seconds(15);
constexpr auto kResponsePollInterval = std::chrono::milliseconds(50);
constexpr auto kResponseTimeout = std::chrono::seconds(10);

// Poll interval/timeout for waiting on the machine's own BUSY indicator
// (see waitUntilReady()) before a `type` or `check` script line -- unlike
// the other timeouts above, this has to tolerate a real program running
// for minutes (e.g. a memory test), not just a FIFO round-trip.
constexpr auto kReadyPollInterval = std::chrono::milliseconds(250);
constexpr auto kReadyTimeout = std::chrono::minutes(10);

void fail(const std::string& msg) {
  std::fprintf(stderr, "pc1500preset: %s\n", msg.c_str());
  std::exit(1);
}

// Calls `predicate` every `interval` until it returns true or `timeout`
// elapses. Returns false on timeout without calling `predicate` again.
bool pollUntil(std::chrono::steady_clock::duration timeout,
               std::chrono::steady_clock::duration interval,
               const std::function<bool()>& predicate) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(interval);
  }
  return false;
}

std::string hex16(uint16_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%04X", v);
  return buf;
}

// Indexed by RomModuleAttachment::slot (0-3).
constexpr const char* kSlotCommands[] = {"loadrommodule", "loadrommodule2", "loadrommodule3",
                                          "loadrommodule4"};

// PC-1500 5x7 dot-matrix glyphs, matching the FIFO `display` command's own
// ASCII-art convention ('#' = lit dot, '.' = blank), confirmed by directly
// reading the live display after a BASIC direct-mode "0" (the ROM's own
// zero-with-a-slash glyph, distinguishing it from the letter 'O'). `check`
// works by encoding its expected text this way and searching the dot
// matrix for a match, since there's no decoder the other way -- the
// display's raw text buffer (`displaytext`) only reflects the last typed
// input line, not a statement's actual evaluated/rendered output (see
// docs/preset_file_format.md's "check" section for the full story). Only
// "0" has a known glyph so far -- preset_file.cpp's parser rejects any
// other `check` value until a new one is added here.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr const char* kGlyphZero[kGlyphHeight] = {
    ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.",
};

// Splits the FIFO `display` command's response into its 7 dot-matrix rows.
std::vector<std::string> splitDisplayRows(const std::string& display) {
  std::vector<std::string> rows;
  std::istringstream iss(display);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    rows.push_back(line);
  }
  return rows;
}

// Searches `rows` for `glyph` at any horizontal column offset --
// position-independent, since where a result renders (start vs. end of
// the line) depends on which BASIC statement produced it, not something
// `check` should have to know.
bool displayContainsGlyph(const std::vector<std::string>& rows,
                           const char* const (&glyph)[kGlyphHeight]) {
  if (static_cast<int>(rows.size()) < kGlyphHeight) return false;
  int width = static_cast<int>(rows[0].size());
  for (int col = 0; col + kGlyphWidth <= width; col++) {
    bool match = true;
    for (int row = 0; row < kGlyphHeight && match; row++) {
      if (static_cast<int>(rows[row].size()) < col + kGlyphWidth ||
          rows[row].compare(col, kGlyphWidth, glyph[row]) != 0) {
        match = false;
      }
    }
    if (match) return true;
  }
  return false;
}

// Writes one line to the emulator's command FIFO. Blocks briefly if the
// FIFO's internal pipe buffer is full (extremely unlikely for these short
// commands) -- the reader (the emulator's own main loop) is already known
// to be attached by the time this is called (see waitForFifoReady).
void sendCommand(const std::string& line) {
  int fd = open(kCommandFifoPath, O_WRONLY);
  if (fd < 0) fail("could not open command FIFO for writing: " + std::string(std::strerror(errno)));
  std::string withNewline = line + "\n";
  ssize_t n = write(fd, withNewline.data(), withNewline.size());
  close(fd);
  if (n < 0 || static_cast<size_t>(n) != withNewline.size()) {
    fail("short write to command FIFO for '" + line + "'");
  }
}

// Sends a command that's known to write a result to the response file
// (see README.md's "Query commands write their result to a response file"),
// and waits for it to appear. Removes any stale response file first so a
// leftover from a previous run/command can't be misread as this one's
// answer -- safe because this tool is the sole driver of the child process
// it just launched (the emulator's FIFO/response paths are fixed, global,
// and single-reader by design, same limitation the manual FIFO workflow in
// README.md already has).
std::string sendCommandForResponse(const std::string& line) {
  std::error_code ec;
  std::filesystem::remove(kResponsePath, ec);
  sendCommand(line);
  std::string response;
  bool gotResponse = pollUntil(kResponseTimeout, kResponsePollInterval, [&] {
    std::ifstream f(kResponsePath, std::ios::binary);
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    response = buf.str();
    return true;
  });
  if (!gotResponse) fail("timed out waiting for a response to '" + line + "'");
  return response;
}

void requireOk(const std::string& response, const std::string& context) {
  if (response.rfind("OK", 0) != 0) {
    fail(context + " failed: " + response);
  }
}

// Waits for the just-launched emulator to have its command FIFO open for
// reading (mkfifo() + open(O_RDONLY) happen near the end of its own
// startup, right before it enters the main loop -- see main.cpp's own
// comment on kCommandFifoPath). Detected by retrying a non-blocking write
// open: it fails with ENXIO until a reader is attached, and succeeds (or
// fails some other way) once one is. Also bails out early, rather than
// spinning for the full timeout, if the child has already exited (e.g. a
// bad ROM path) -- otherwise this would misreport that as a plain timeout.
void waitForFifoReady(pid_t childPid) {
  bool ready = pollUntil(kFifoReadyTimeout, kFifoReadyPollInterval, [&] {
    int status = 0;
    pid_t r = waitpid(childPid, &status, WNOHANG);
    if (r == childPid) {
      fail("pc1500emu exited before its command FIFO became ready (check the ROM path and any "
           "stderr output above)");
    }
    int fd = open(kCommandFifoPath, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
      close(fd);
      return true;
    }
    if (errno != ENXIO && errno != ENOENT) {
      fail("could not probe command FIFO '" + std::string(kCommandFifoPath) +
           "': " + std::strerror(errno));
    }
    return false;
  });
  if (!ready) fail("timed out waiting for pc1500emu's command FIFO to become ready");
}

pid_t launchEmulator(const std::string& emulatorPath, const std::string& firmwarePath) {
  // --no-state: a preset always cold-boots from its own firmware/hardware
  // sections, same as the in-process applier -- never resume whatever
  // state file the emulator's own --conf might otherwise auto-load.
  char* argv[] = {const_cast<char*>(emulatorPath.c_str()), const_cast<char*>("--no-state"),
                   const_cast<char*>(firmwarePath.c_str()), nullptr};

  pid_t pid = 0;
  int rc = posix_spawn(&pid, emulatorPath.c_str(), nullptr, nullptr, argv, environ);
  if (rc != 0) {
    fail("could not launch '" + emulatorPath + "': " + std::strerror(rc));
  }
  return pid;
}

// SIGTERM first (lets the emulator's own SDL2/window teardown run cleanly),
// falling back to SIGKILL if it hasn't exited after a short grace period --
// used to end a preset run that included `check` lines (a test run) rather
// than leaving the emulator open forever, unlike the interactive-use case
// (see main()'s call site).
void terminateEmulator(pid_t childPid) {
  kill(childPid, SIGTERM);
  bool exited = pollUntil(std::chrono::seconds(3), std::chrono::milliseconds(50), [&] {
    int status = 0;
    return waitpid(childPid, &status, WNOHANG) == childPid;
  });
  if (!exited) {
    kill(childPid, SIGKILL);
    waitpid(childPid, nullptr, 0);
  }
}

// Polls `status` until the machine's own BUSY indicator (ind1(764E) bit 0,
// see ../pc1500emu's own formatRegisters()) clears, i.e. it has settled
// back into its idle/READY prompt. `typeline` steps the CPU directly
// rather than queuing into the normal keyboard path (see runScriptLine's
// kType case), so it isn't safe to send while a program -- e.g. a
// minutes-long memory test -- still has the CPU; this is what lets a
// `type`/`check` line proceed the moment the machine is actually ready
// instead of guessing a fixed real-time delay.
void waitUntilReady() {
  bool ready = pollUntil(kReadyTimeout, kReadyPollInterval, [&] {
    std::string status = sendCommandForResponse("status");
    size_t pos = status.find("busy=");
    if (pos == std::string::npos || pos + 5 >= status.size()) {
      fail("could not find 'busy=' in the emulator's status response: " + status);
    }
    return status[pos + 5] != '1';
  });
  if (!ready) fail("timed out waiting for the emulator to become ready (BUSY indicator never cleared)");
}

// Tallies `check` script lines across the whole run -- drives both the
// final summary line and main()'s exit code.
struct RunResult {
  int checksTotal = 0;
  int checksFailed = 0;
};

void runScriptLine(const ScriptLine& line, RunResult* result) {
  switch (line.kind) {
    case ScriptLineKind::kKeyPress:
      // Forward verbatim -- the FIFO `key` command already parses an
      // optional "shift+" prefix itself, same vocabulary this preset format
      // uses.
      sendCommand("key " + line.text);
      std::this_thread::sleep_for(kPostStatementSettle);
      break;

    case ScriptLineKind::kType: {
      // `typeline` (unlike the raw `type` command) types synchronously via
      // direct cycle-stepping and presses Enter itself -- exactly the
      // "one complete, submitted statement" semantics a preset script's
      // `type` line wants (see docs/preset_file_format.md). Wait for the
      // machine to actually be idle first (see waitUntilReady()).
      waitUntilReady();
      if (line.text.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos) {
        std::string response = sendCommandForResponse("typeline " + line.text);
        requireOk(response, "'type " + line.text + "'");
      } else {
        // The ROM's keyboard dispatch only knows physical keys, not case --
        // there is one key per letter, and `typeline` (via the emulator's
        // own charToTapActions) folds any lowercase input to that same
        // uppercase key. Real (and interactive-emulator) lowercase input
        // instead comes from a one-shot Shift-tap immediately before the
        // letter -- the emulator's FIFO `key shift+<letter>` command already
        // does exactly this (see its own handler in pc1500emu's main.cpp),
        // so a line containing lowercase letters is split into runs: each
        // non-lowercase run is sent whole via `typelinenoenter` (so it
        // accumulates on the same input line without submitting), each
        // lowercase letter is sent individually as `key shift+<letter>`.
        //
        // `key` commands queue into the emulator's own frame-driven action
        // queue instead of executing synchronously (unlike
        // `typelinenoenter`, which steps the CPU directly within the FIFO
        // handler and blocks the emulator's main loop -- and with it, that
        // queue's own draining -- until it's done). `kPostStatementSettle`
        // after *every* `key shift+<letter>` push, not just once per run of
        // several, gives the queue real time to actually drain before the
        // next command runs -- confirmed empirically that skipping this
        // between consecutive shifted letters (an earlier version of this
        // code settled only once, after the last of a run) leaves later
        // taps still queued when the next synchronous `typelinenoenter`
        // fragment fires, corrupting/reordering the input (e.g. "Bank"
        // typed as "Ban* 0"k" -- the still-queued 'k' tap leaking out after
        // the following fragment's own keystrokes). Same reasoning as
        // kKeyPress's own settle above, just needed after each letter here
        // too rather than only between chunks.
        std::string run;
        auto flushRun = [&]() {
          if (run.empty()) return;
          std::string response = sendCommandForResponse("typelinenoenter " + run);
          requireOk(response, "'type " + line.text + "'");
          run.clear();
        };
        for (char c : line.text) {
          if (c >= 'a' && c <= 'z') {
            flushRun();
            sendCommand("key shift+" + std::string(1, c));
            std::this_thread::sleep_for(kPostStatementSettle);
          } else {
            run.push_back(c);
          }
        }
        flushRun();
        sendCommand("key ent");
      }
      std::this_thread::sleep_for(kPostStatementSettle);
      break;
    }

    case ScriptLineKind::kWait: {
      std::printf("pc1500preset: waiting %.3gs...\n", line.waitSeconds);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<long long>(line.waitSeconds * 1000)));
      break;
    }

    case ScriptLineKind::kCheck: {
      // "wait for ready, then read the display and compare" -- see
      // docs/preset_file_format.md's "check" section. preset_file.cpp's
      // parser only accepts line.text == "0" today (the only expected
      // value with a known glyph encoding -- see kGlyphZero above), so
      // this always checks for the zero glyph; extend both together to
      // support more.
      waitUntilReady();
      std::vector<std::string> rows = splitDisplayRows(sendCommandForResponse("display"));
      bool pass = displayContainsGlyph(rows, kGlyphZero);
      result->checksTotal++;
      if (!pass) result->checksFailed++;
      std::printf("pc1500preset: check %s -- expected '%s' glyph %s on display\n",
                  pass ? "PASSED" : "FAILED", line.text.c_str(), pass ? "found" : "not found");
      break;
    }
  }
}

void runScript(const std::vector<ScriptLine>& lines, RunResult* result) {
  for (const ScriptLine& line : lines) runScriptLine(line, result);
}

void applyPreset(const PresetFile& preset, RunResult* result) {
  // Must precede setextram: the FIFO's own setmachine handler notes that
  // it changes how setextram's 0x4800 window is interpreted (0x4800-based
  // on a PC-1500, 0x5800-based on a PC-1500A -- see pc1500emu's
  // Bus::extRamExtBase()).
  sendCommand("setmachine " + std::string(preset.model == "PC-1500A" ? "1500a" : "1500"));

  if (preset.extRam0000Bytes) {
    sendCommand("setextram 0000 " + std::to_string(*preset.extRam0000Bytes));
  }
  if (preset.extRam4800Bytes) {
    sendCommand("setextram 4800 " + std::to_string(*preset.extRam4800Bytes));
  }
  if (preset.ce163Enabled) {
    sendCommand("setce163 1");
  }
  if (preset.ce168nBanks) {
    sendCommand("setce168n " + std::to_string(*preset.ce168nBanks) + " " +
                std::to_string(preset.ce168nFirstRoBank));
  }

  for (const auto& m : preset.romModules) {
    std::string cmd = std::string(kSlotCommands[m.slot]) + " " + hex16(m.base) + " " +
                       (m.requirePv ? "1" : "0") + " " + (m.usePuBank ? "1" : "0") + " " + m.path;
    std::string response = sendCommandForResponse(cmd);
    requireOk(response, "rom-module '" + m.path + "'");
  }

  sendCommand("reset");

  if (!preset.preLoadKeys.empty() || !preset.postLoadKeys.empty() ||
      !preset.ce168nBankContent.empty()) {
    std::this_thread::sleep_for(kBootSettle);
  }

  for (const auto& c : preset.ce168nBankContent) {
    sendCommand("poke " + hex16(0x5800 + c.bank) + " 00");
    std::string response = sendCommandForResponse("loadbinary 0000 " + c.path);
    requireOk(response, "ce168n bank-content '" + c.path + "'");
  }

  runScript(preset.preLoadKeys, result);

  if (preset.hasProgram) {
    // `program.text` (inline BASIC) has no FIFO command of its own --
    // loadbasictext only takes a path -- so it's spilled to a temp file
    // first and cleaned up right after, regardless of outcome.
    std::string programPath = preset.programPath;
    std::filesystem::path tempPath;
    if (preset.programText) {
      tempPath = std::filesystem::temp_directory_path() /
                 ("pc1500preset-inline-" + std::to_string(getpid()) + ".bas");
      std::ofstream tempFile(tempPath, std::ios::binary);
      tempFile << *preset.programText;
      tempFile.close();
      programPath = tempPath.string();
    }

    std::string response;
    std::string label = preset.programText ? "program (inline text)" : "program '" + programPath + "'";
    switch (preset.programFormat) {
      case ProgramFormat::kBinary:
        response =
            sendCommandForResponse("loadbinary " + hex16(preset.loadAddress) + " " + programPath);
        break;
      case ProgramFormat::kBasicTokenized:
        response = sendCommandForResponse("loadbasic " + programPath);
        break;
      case ProgramFormat::kBasicText:
        response = sendCommandForResponse("loadbasictext " + programPath);
        break;
    }
    if (!tempPath.empty()) std::filesystem::remove(tempPath);
    requireOk(response, label);
  }

  runScript(preset.postLoadKeys, result);
}

void printUsage(const char* argv0) {
  std::printf(
      "Usage: %s [--emulator <path>] <preset-file>\n"
      "\n"
      "Launches an unmodified pc1500emu (see 'Locating the emulator binary'\n"
      "below) and applies a .pc1500 file to it entirely through its\n"
      "existing FIFO scripting interface -- firmware, memory expansion, ROM\n"
      "modules, a program to load, and pre/post-load keystroke scripts. See\n"
      "docs/preset_file_format.md for the file format. The emulator process\n"
      "is left running afterward for interactive use.\n"
      "\n"
      "A script's 'check <expected text>' lines wait for the machine to\n"
      "become idle, compare its display against <expected text>, and are\n"
      "tallied into an exit code: 0 if every check passed (or the script had\n"
      "none), 1 if any failed.\n"
      "\n"
      "  --emulator <path>   Path to the pc1500emu executable to launch.\n"
      "  -h, --help           Show this help and exit.\n"
      "\n"
      "Locating the emulator binary, when --emulator isn't given:\n"
      "  1. $PC1500EMU_PATH, if set.\n"
      "  2. ../pc1500emu/build/src/host/pc1500emu, relative to the\n"
      "     current directory (the sibling pc1500emu checkout's own build).\n"
      "  3. 'pc1500emu' next to this tool's own executable.\n"
      "  4. 'pc1500emu' on PATH.\n",
      argv0);
}

// See printUsage's own "Locating the emulator binary" list -- this mirrors
// it exactly, in the same order.
std::string defaultEmulatorPath(const char* argv0) {
  if (const char* envPath = std::getenv("PC1500EMU_PATH")) {
    if (*envPath) return envPath;
  }

  std::filesystem::path sibling =
      std::filesystem::path("../pc1500emu/build/src/host/pc1500emu");
  if (std::filesystem::exists(sibling)) return sibling.string();

  std::filesystem::path self(argv0);
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::canonical(self, ec);
  std::filesystem::path dir = ec ? self.parent_path() : resolved.parent_path();
  std::filesystem::path nextToSelf = dir / "pc1500emu";
  if (std::filesystem::exists(nextToSelf)) return nextToSelf.string();

  return "pc1500emu";  // fall back to PATH lookup
}

}  // namespace

int main(int argc, char** argv) {
  std::string emulatorPath;
  std::string presetPath;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "--emulator") {
      if (i + 1 >= argc) fail("--emulator requires a path argument");
      emulatorPath = argv[++i];
    } else if (presetPath.empty()) {
      presetPath = arg;
    } else {
      fail("unexpected extra argument '" + arg + "'");
    }
  }
  if (presetPath.empty()) {
    printUsage(argv[0]);
    return 1;
  }
  if (emulatorPath.empty()) emulatorPath = defaultEmulatorPath(argv[0]);

  PresetFile preset;
  std::string error;
  if (!pc1500preset::parsePresetFile(presetPath, &preset, &error)) {
    fail(error);
  }

  std::printf("pc1500preset: launching '%s' with firmware '%s'...\n", emulatorPath.c_str(),
              preset.firmwarePath.c_str());
  pid_t childPid = launchEmulator(emulatorPath, preset.firmwarePath);
  waitForFifoReady(childPid);

  std::string automationResponse = sendCommandForResponse("automation on");
  if (automationResponse != "ON") {
    fail("could not enable automation mode (got '" + automationResponse + "')");
  }

  RunResult result;
  applyPreset(preset, &result);

  if (result.checksTotal > 0) {
    // A preset with `check` lines is a test run, not a demo/interactive
    // one -- terminate the emulator once the script (including every
    // check) has finished, rather than leaving it running forever (see
    // terminateEmulator()'s own comment).
    terminateEmulator(childPid);
    std::printf("pc1500preset: preset applied, pc1500emu (pid %d) terminated.\n",
                static_cast<int>(childPid));
    std::printf("pc1500preset: %d/%d checks passed.\n", result.checksTotal - result.checksFailed,
                result.checksTotal);
  } else {
    // Left running for interactive use -- automation mode was only ever
    // needed to keep a stray real keypress from corrupting the scripted
    // load above (see README's automation-mode note), so turn it back off
    // now that the script is done and hand the keyboard back to the user.
    sendCommand("automation off");
    std::printf("pc1500preset: preset applied, pc1500emu (pid %d) left running.\n",
                static_cast<int>(childPid));
  }
  return (result.checksFailed > 0) ? 1 : 0;
}
