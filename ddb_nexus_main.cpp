// ddb-nexus: builds a Nexus (.nxs / .nxz) file from a 3D model, in its own process.
//
//   ddb-nexus <input> <output>
//   ddb-nexus --version
//
// DroneDB runs this program instead of loading libnxs into its own process, so the
// Nexus, VCGLib and Qt code stays on the far side of a process boundary. The build
// itself is the single nexusBuild() call of the C API (nxs.h): the output is exactly
// what an in-process call produces.
//
// While the build runs, one line per kHeartbeatInterval is written to stdout, so a
// supervisor that watches the program's output can tell a long build from a hang.
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
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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

void printUsage(std::ostream& out) {
    out << "Usage: " << kProgram << " <input> <output>\n"
        << "       " << kProgram << " --version\n"
        << "Builds a Nexus file (.nxs, or .nxz when <output> ends in .nxz) from an OBJ or PLY model.\n";
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

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << kProgram << " " << DDB_NEXUS_VERSION << std::endl;
        return kExitOk;
    }

    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        printUsage(std::cout);
        return kExitOk;
    }

    if (argc != 3) {
        printUsage(std::cerr);
        return kExitUsage;
    }

    const char* input = argv[1];
    const char* output = argv[2];

    char errorMessage[kErrorMessageSize] = {0};
    NXSErr result;
    {
        Heartbeat heartbeat(input);
        result = nexusBuild(input, output, errorMessage, kErrorMessageSize);
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
