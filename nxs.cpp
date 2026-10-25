// libnexus - C API and ddb-nexus command line tool to generate Nexus files.
// Copyright (C) Digipa. Licensed under the GNU General Public License, version 3
// (see LICENSE). It builds on Nexus (GPL-2.0-or-later), VCGLib (GPL-3.0) and Qt (LGPL-3.0).

#include "nxs.h"

#include <locale.h>

#include <QCoreApplication>
#include <QImageReader>
#include <QLocale>
#include <QtPlugin>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../common/error.h"
#include "../common/pathutil.h"
#include "../common/traversal.h"
#include "../nxsbuild/nexusbuilder.h"
#include "../nxsedit/extractor.h"
#include "kdtree.h"
#include "meshstream.h"
#include "objloader.h"
#include "plyloader.h"
#include "tsploader.h"

using namespace std;
using namespace nx;

#ifdef INITIALIZE_STATIC_LIBJPEG
Q_IMPORT_PLUGIN(QJpegPlugin);
#endif

// =====================================================================
// Internal helpers (RAII, Qt init, locale management)
// =====================================================================

namespace {

void setErrorMessage(char* buffer, int bufferSize, const std::string& message) {
    if (!buffer || bufferSize <= 0)
        return;
    // The messages are UTF-8 like every other message and path of this API
    std::strncpy(buffer, message.c_str(), bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
}

// RAII: set LC_NUMERIC to "C" during this scope and restore previous value.
class NumericLocaleGuard {
public:
    NumericLocaleGuard() {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        if (current) {
            oldLocale_ = current;
        }
        std::setlocale(LC_NUMERIC, "C");
    }

    ~NumericLocaleGuard() {
        if (!oldLocale_.empty()) {
            std::setlocale(LC_NUMERIC, oldLocale_.c_str());
        }
    }

private:
    std::string oldLocale_;
};

// RAII: set QLocale::default to C for this scope and restore previous.
class QLocaleGuard {
public:
    QLocaleGuard() : oldLocale_(QLocale()) { QLocale::setDefault(QLocale::c()); }

    ~QLocaleGuard() { QLocale::setDefault(oldLocale_); }

private:
    QLocale oldLocale_;
};

// RAII: remove a file on destruction if path is not empty.
class TempFileGuard {
public:
    TempFileGuard() = default;
    explicit TempFileGuard(const std::string& path) : path_(path) {}

    void setPath(const std::string& path) { path_ = path; }
    const std::string& path() const { return path_; }

    ~TempFileGuard() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(pathFromUtf8(path_), ec);
        }
    }

private:
    std::string path_;
};

// Initialize Qt core application and plugin paths once, in a thread-safe way.
void initializeQtEnvironment() {
    static std::once_flag qtInitFlag;

    std::call_once(qtInitFlag, []() {
        // If the host process is already a Qt application, do not create another instance.
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "nexusBuild";
            static char* argv[] = {appName, nullptr};

            // Never deleted, on purpose. A static QCoreApplication would be destroyed by exit()
            // on the thread that ends the process, but it is created by the first build, which
            // a host (a service running its jobs on worker threads) runs on a thread that is
            // long gone by then: its destructor reads the dead thread's data and the process
            // crashes (SIGSEGV) after the work was done. The operating system reclaims the
            // instance when the process ends.
            new QCoreApplication(argc, argv);
        }

        // Plugins (image formats) are looked for in a "plugins" folder next to the library.
        std::string pluginPath;
#ifdef WIN32
        // On Windows, derive plugin path from this DLL location.
        HMODULE hModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&initializeQtEnvironment),
                               &hModule)) {
            wchar_t dllPath[MAX_PATH] = {};
            if (GetModuleFileNameW(hModule, dllPath, MAX_PATH) > 0)
                pluginPath = pathToUtf8(std::filesystem::path(dllPath).parent_path() / "plugins");
        }
#else
        // On Unix-like systems, fall back to the application directory.
        pluginPath = QCoreApplication::applicationDirPath().toStdString() + "/plugins";
#endif
        if (!pluginPath.empty() && pathExists(pluginPath))
            QCoreApplication::addLibraryPath(QString::fromUtf8(pluginPath.c_str()));
    });
}

// Builds the Nexus file; throws on failure.
void buildNexusFile(const std::string& input,
                    const std::string& output,
                    const NexusBuildOptions& opts) {
    const uint64_t max_memory = (uint64_t(1) << 20) * static_cast<uint64_t>(opts.ram_buffer_mb) / 4;

    std::vector<std::string> inputs;
    inputs.push_back(input);

    vcg::Point3d origin(0, 0, 0);

    std::unique_ptr<Stream> stream;
    std::unique_ptr<KDTree> tree;

    // Build a somewhat unique base name for cache files.
    // Place the out-of-core cache (stream + kd-tree) in the same directory as
    // the output, which under the DroneDB build pipeline is the per-build temp
    // folder. This keeps the (potentially multi-GB) scratch on the same large
    // volume as the result and lets it be reclaimed together with that folder,
    // instead of defaulting to the system temp dir. VirtualMemory uses an
    // absolute template prefix as-is.
    using Clock = std::chrono::steady_clock;
    auto now = Clock::now().time_since_epoch().count();
    const std::string cacheDir = absoluteParentPath(output);
    const std::string cacheBase = cacheDir + "/nexus_cache_" + std::to_string(static_cast<unsigned long long>(now));

    // Create and load mesh stream
    {
        auto streamImpl = std::make_unique<StreamSoup>(cacheBase + "_stream");
        streamImpl->setVertexQuantization(opts.vertex_quantization);
        streamImpl->setMaxMemory(max_memory);
        streamImpl->origin = origin;

        std::string mtl;  // not exposed yet
        streamImpl->load(inputs, mtl);

        stream = std::move(streamImpl);
    }

    const bool has_colors = stream->hasColors();
    const bool has_normals = stream->hasNormals();
    const bool has_textures = stream->hasTextures();

    uint32_t components = 0;
    if (!opts.point_cloud)
        components |= NexusBuilder::FACES;

    if ((!opts.disable_normals && (!opts.point_cloud || has_normals)) || opts.force_normals) {
        components |= NexusBuilder::NORMALS;
    }

    if ((has_colors && !opts.disable_colors) || opts.force_colors) {
        components |= NexusBuilder::COLORS;
    }

    if (has_textures && !opts.disable_texcoords) {
        components |= NexusBuilder::TEXTURES;
    }

    // Do not keep textures if they are not going to be used
    if (!(components & NexusBuilder::TEXTURES)) {
        stream->textures.clear();
    }

    // Place the builder's out-of-core caches (chunks + per-node texture file)
    // in the same directory as the output (the per-build temp folder), so they
    // do not leak into the process working directory and are reclaimed with it.
    NexusBuilder builder(components, cacheDir);
    builder.skipSimplifyLevels = opts.skip_levels;
    builder.setMaxMemory(max_memory);

    builder.setScaling(opts.scaling);
    builder.useNodeTex = !opts.use_original_textures;
    builder.createPowTwoTex = opts.create_pow_two_textures;

    if (opts.deepzoom) {
        builder.header.signature.flags |= nx::Signature::Flags::DEEPZOOM;
    }

    builder.tex_quality = opts.texture_quality;

    if (!builder.initAtlas(stream->textures)) {
        throw Error("Failed to initialize texture atlas");
    }

    // Create KD-tree
    {
        auto treeImpl = std::make_unique<KDTreeSoup>(cacheBase + "_tree", opts.adaptive);
        treeImpl->setMaxMemory((uint64_t(1) << 20) * static_cast<uint64_t>(opts.ram_buffer_mb) / 2);

        treeImpl->setMaxWeight(opts.node_faces);
        treeImpl->texelWeight = opts.texel_weight;
        treeImpl->setTrianglesPerBlock(opts.node_faces);

        tree = std::move(treeImpl);
    }

    // Build the hierarchy
    builder.create(tree.get(), stream.get(), opts.top_node_faces);

    // Output handling (.nxs vs .nxz)
    const std::string& finalOutput = output;
    bool hasNxzExtension = endsWithNoCase(finalOutput, ".nxz");
    bool doCompression = hasNxzExtension && opts.enable_compression;

    std::string builderOutput = finalOutput;
    if (doCompression) {
        // If compression is enabled, write a temporary .nxs file first
        builderOutput += ".tmp.nxs";
    }

    builder.save(builderOutput);

    if (doCompression) {
        // Ensure temporary .nxs is always removed
        TempFileGuard tempNxsGuard(builderOutput);

        NexusData nexus;  // its constructor creates and owns the file reader

        if (!nexus.open(builderOutput.c_str())) {
            throw Error("Could not open file: " + builderOutput);
        }

        // Validate compression library
        Signature signature = nexus.header.signature;
        signature.flags &= ~(Signature::MECO | Signature::CORTO);

        if (opts.compress_lib == "meco") {
            signature.flags |= Signature::MECO;
        } else if (opts.compress_lib == "corto") {
            signature.flags |= Signature::CORTO;
        } else {
            throw Error("Unknown compression method: " + opts.compress_lib);
        }

        Extractor extractor(&nexus);

        // Determine coord_step
        float coord_step = opts.coord_step;
        if (coord_step > 0.0f) {
            extractor.error_factor = 0.0f;
        } else if (opts.position_bits > 0) {
            vcg::Sphere3f& sphere = nexus.header.sphere;
            coord_step = sphere.Radius() / std::pow(2.0f, opts.position_bits);
            extractor.error_factor = 0.0f;
        } else if (opts.error_q > 0.0f) {
            uint32_t sink = nexus.header.n_nodes - 1;
            coord_step = opts.error_q * nexus.nodes[0].error / 2.0f;
            for (uint32_t i = 0; i < sink; ++i) {
                Node& n = nexus.nodes[i];
                Patch& patch = nexus.patches[n.first_patch];
                if (patch.node != sink)
                    continue;
                double e = opts.error_q * n.error / 2.0;
                if (e < coord_step && e > 0.0)
                    coord_step = static_cast<float>(e);
            }
            extractor.error_factor = opts.error_q;
        }

        // Fallback to avoid log2(0) if coord_step is still <= 0
        if (coord_step <= 0.0f) {
            coord_step = 1.0f;
            extractor.error_factor = 0.0f;
        }

        extractor.coord_q = static_cast<int>(std::log2(coord_step));
        extractor.norm_bits = opts.normal_bits;

        extractor.color_bits[0] = opts.luma_bits;
        extractor.color_bits[1] = opts.chroma_bits;
        extractor.color_bits[2] = opts.chroma_bits;
        extractor.color_bits[3] = opts.alpha_bits;

        extractor.tex_step = opts.tex_step;

        // Save final compressed file (.nxz)
        extractor.save(finalOutput, signature);
    }
}

// Internal implementation used by both nexusBuild and nexusBuildEx.
NXSErr doNexusBuild(const char* input,
                    const char* output,
                    const NexusBuildOptions& opts,
                    char* errorMessage,
                    int errorMessageSize) {
    // Basic parameter checks
    if (!input || !output) {
        setErrorMessage(errorMessage, errorMessageSize, "Input or output parameter is null");
        return NXSERR_INVALID_INPUT;
    }

    if (!pathExists(input)) {
        setErrorMessage(errorMessage,
                        errorMessageSize,
                        std::string("Input file does not exist: ") + input);
        return NXSERR_INVALID_INPUT;
    }

    try {
        // Initialize Qt (thread-safe, once per process)
        initializeQtEnvironment();

        // Use "C" numeric locale for this operation only.
        NumericLocaleGuard numericLocaleGuard;
        QLocaleGuard qtLocaleGuard;

#if defined(DEBUG)
        std::cerr << "Supported image formats:";
        for (const auto& format : QImageReader::supportedImageFormats())
            std::cerr << " " << format.constData();
        std::cerr << std::endl;
#endif

        try {
            buildNexusFile(input, output, opts);
        } catch (const TexturePackingError& error) {
            // A node needs a texture too large to pack. Models of this kind used to build
            // without textures, so they still do: the first attempt left nothing behind (it
            // failed while building the hierarchy, and its scratch files went with it).
            if (opts.disable_texcoords)
                throw;
            std::cerr << "Warning: " << error.what() << std::endl;
            NexusBuildOptions withoutTextures = opts;
            withoutTextures.disable_texcoords = true;
            buildNexusFile(input, output, withoutTextures);
            std::cerr << "Warning: textures could not be packed; built without textures" << std::endl;
        }

        return NXSERR_NONE;
    } catch (const nx::Error& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, e.what());
        return NXSERR_EXCEPTION;
    } catch (const char* error) {
        // corto, the mesh codec, throws its errors as C strings
        std::cerr << "Fatal error (const char*): " << error << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, std::string(error));
        return NXSERR_EXCEPTION;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error (std::exception): " << e.what() << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, std::string("Exception: ") + e.what());
        return NXSERR_EXCEPTION;
    } catch (...) {
        std::cerr << "Fatal error: unknown exception in nexusBuild()" << std::endl;
        setErrorMessage(errorMessage,
                        errorMessageSize,
                        std::string("Unknown exception in nexusBuild"));
        return NXSERR_EXCEPTION;
    }
}

}  // namespace


// =====================================================================
// Public API
// =====================================================================

// New extended function using NexusBuildOptions.
NXS_DLL NXSErr nexusBuildEx(const char* input,
                            const char* output,
                            const NexusBuildOptions& options,
                            char* errorMessage,
                            int errorMessageSize) {
    return doNexusBuild(input, output, options, errorMessage, errorMessageSize);
}

// Original function signature kept for backwards compatibility.
// Uses default NexusBuildOptions.
NXS_DLL NXSErr nexusBuild(const char* input,
                          const char* output,
                          char* errorMessage,
                          int errorMessageSize) {
    NexusBuildOptions defaultOptions;  // keep current libnexus defaults
    return doNexusBuild(input, output, defaultOptions, errorMessage, errorMessageSize);
}
