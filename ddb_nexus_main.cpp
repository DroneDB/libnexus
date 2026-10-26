// ddb-nexus: builds a Nexus (.nxs / .nxz) file from a 3D model, in its own process.
//
//   ddb-nexus [--threads N] <input> <output>
//   ddb-nexus --version
//
// DroneDB runs this program instead of loading libnxs into its own process, so the
// Nexus and VCGLib code stays on the far side of a process boundary. The build
// itself is the single nexusBuildEx() call of the C API (nxs.h), with the default
// options and at most N threads busy (one per core without --threads): the output is
// exactly what an in-process call produces.
//
// While the build runs, one line per kHeartbeatInterval is written to stdout, so a
// supervisor that watches the program's output can tell a long build from a hang.
//
// Paths are UTF-8 on every platform, which is what nexusBuild() expects. On Windows the
// program reads its UTF-16 command line and converts it, since the narrow argv of main is
// in the ANSI code page and cannot hold a model under, say, a Cyrillic or CJK folder name.
//
// Exit codes:
//   0   success (or --version / --help)
//   1   the build failed (NXSERR_EXCEPTION): the message is on stderr
//   2   the input is invalid, e.g. it does not exist (NXSERR_INVALID_INPUT): message on stderr
//   64  wrong command line (the sysexits.h EX_USAGE value): usage on stderr
//
// Licensed under the GNU General Public License, version 3.

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "nxs.h"

#ifndef DDB_NEXUS_VERSION
#define DDB_NEXUS_VERSION "unknown"
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitBuildFailed = 1;
constexpr int kExitInvalidInput = 2;
constexpr int kExitUsage = 64;

constexpr int kErrorMessageSize = 4096;
constexpr const char* kProgram = "ddb-nexus";

// Well below any idle timeout a supervisor is likely to apply to a silent child.
constexpr std::chrono::seconds kHeartbeatInterval{15};

// More than any machine has cores; a larger value is a mistake, not a request
constexpr int kMaxThreads = 4096;

// A thread count: decimal digits only, from 1 to kMaxThreads; anything else is 0
int parseThreads(const std::string& value) {
    if (value.empty() || value.size() > 4)
        return 0;
    int n = 0;
    for (const char c : value) {
        if (c < '0' || c > '9')
            return 0;
        n = n * 10 + (c - '0');
    }
    return n <= kMaxThreads ? n : 0;
}

void printUsage(std::ostream& out) {
    out << "Usage: " << kProgram << " [--threads N] <input> <output>\n"
        << "       " << kProgram << " --version\n"
        << "Builds a Nexus file (.nxs, or .nxz when <output> ends in .nxz) from an OBJ or PLY model,\n"
        << "keeping at most N threads busy (default: one per core).\n";
}

// Prints "ddb-nexus: building <input> (<n> s)" at once and then every kHeartbeatInterval
// until stopped. Every line is flushed: the reader is usually a pipe.
class Heartbeat {
public:
    explicit Heartbeat(std::string input) : input_(std::move(input)), thread_([this] { run(); }) {}

    ~Heartbeat() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

private:
    void run() {
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        do {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
            std::cout << kProgram << ": building " << input_ << " (" << elapsed.count() << " s)" << std::endl;
        } while (!wake_.wait_for(lock, kHeartbeatInterval, [this] { return stop_; }));
    }

    std::string input_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;  // last: starts running when the members above are ready
};

// args are the command line arguments without the program name, as UTF-8
int run(const std::vector<std::string>& args) {
    if (args.size() == 1 && args[0] == "--version") {
        std::cout << kProgram << " " << DDB_NEXUS_VERSION << std::endl;
        return kExitOk;
    }

    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        printUsage(std::cout);
        return kExitOk;
    }

    NexusBuildOptions options;
    std::vector<std::string> paths(args);
    if (paths.size() == 4 && paths[0] == "--threads") {
        const int threads = parseThreads(paths[1]);
        if (threads == 0) {
            std::cerr << kProgram << ": --threads takes a whole number from 1 to " << kMaxThreads << ", not '"
                      << paths[1] << "'" << std::endl;
            printUsage(std::cerr);
            return kExitUsage;
        }
        options.threads = threads;
        paths.erase(paths.begin(), paths.begin() + 2);
    }

    if (paths.size() != 2) {
        printUsage(std::cerr);
        return kExitUsage;
    }

    const char* input = paths[0].c_str();
    const char* output = paths[1].c_str();

    char errorMessage[kErrorMessageSize] = {0};
    NXSErr result;
    {
        Heartbeat heartbeat(input);
        result = nexusBuildEx(input, output, options, errorMessage, kErrorMessageSize);
    }

    if (result == NXSERR_NONE)
        return kExitOk;

    const char* message = errorMessage[0] != '\0' ? errorMessage : "no details available";
    if (result == NXSERR_INVALID_INPUT) {
        std::cerr << kProgram << ": invalid input: " << message << std::endl;
        return kExitInvalidInput;
    }

    std::cerr << kProgram << ": could not build " << output << " from " << input << ": " << message << std::endl;
    return kExitBuildFailed;
}

#ifdef _WIN32
// A UTF-16 command line argument as UTF-8
std::string toUtf8(const wchar_t* wide) {
    const int length = static_cast<int>(std::wcslen(wide));
    if (length == 0)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, &out[0], size, nullptr, nullptr);
    return out;
}
#endif

}  // namespace

#ifdef _WIN32

// The arguments arrive as UTF-16 so that a path in any script survives
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(toUtf8(argv[i]));
    return run(args);
}

#else

int main(int argc, char* argv[]) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}

#endif
