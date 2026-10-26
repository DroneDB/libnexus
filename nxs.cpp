// libnexus - C API and ddb-nexus command line tool to generate Nexus files.
// Copyright (C) Digipa. Licensed under the GNU General Public License, version 3
// (see LICENSE). It builds on Nexus (GPL-2.0-or-later) and VCGLib (GPL-3.0).

#include "nxs.h"

#include <locale.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// =====================================================================
// Internal helpers (RAII, locale management)
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

// RAII: remove a file on destruction if path is not empty.
class TempFileGuard {
public:
    TempFileGuard() = default;
    explicit TempFileGuard(const std::string& path) : path_(path) {}

    void setPath(const std::string& path) { path_ = path; }
    const std::string& path() const { return path_; }
    // The file is complete: leave it in place
    void keep() { path_.clear(); }

    ~TempFileGuard() {
        if (path_.empty())
            return;
        // A destructor must not throw, and the path conversion can (see doNexusBuild). A folder
        // is never what the guard is for: it is the path of an output that could not be opened.
        try {
            const auto path = nativePath(path_);
            std::error_code ec;
            if (!std::filesystem::is_directory(path, ec))
                std::filesystem::remove(path, ec);
        } catch (...) {
        }
    }

private:
    std::string path_;
};

// The share of a node texture that the texture charts of the node's triangles fill once packed.
// On a photogrammetry model (224k triangles, 1.6 gigapixels of texture) nodes cut at this share
// of the largest node texture left 4% of the texels to the downscaling that makes them fit.
constexpr double NODE_TEXTURE_FILL = 0.35;

// The weight of a texel in the kd-tree, which splits a node when the weight of its texels passes
// node_faces: a node holds the texels that fill its texture without downscaling, at most
// max_node_texture_side on a side. 0 (no texel limit) when there is no such side.
float texelWeight(const NexusBuildOptions& opts) {
    const double side = double(opts.max_node_texture_side);
    if (side <= 0)
        return 0.0f;
    return float(double(opts.node_faces) / (side * side * NODE_TEXTURE_FILL));
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

    // Remove the temporary files the saves below write, whatever happens. They are declared
    // before the builder, which keeps its file open until it is destroyed when its save throws:
    // on Windows an open file cannot be removed.
    TempFileGuard writtenOutputGuard;
    TempFileGuard temporaryNxsGuard;

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

    builder.texFormat = NexusBuilder::TextureFormat::Webp;
    builder.tex_quality = opts.texture_quality;
    builder.textureThreads = opts.threads > 0 ? opts.threads
                                              : int(std::max(1u, std::thread::hardware_concurrency()));
    builder.maxNodeTexSide = opts.max_node_texture_side;

    if (!builder.initAtlas(stream->textures)) {
        throw Error("Failed to initialize texture atlas");
    }

    // Create KD-tree
    {
        auto treeImpl = std::make_unique<KDTreeSoup>(cacheBase + "_tree", opts.adaptive);
        treeImpl->setMaxMemory((uint64_t(1) << 20) * static_cast<uint64_t>(opts.ram_buffer_mb) / 2);

        treeImpl->setMaxWeight(opts.node_faces);
        treeImpl->texelWeight = texelWeight(opts);
        treeImpl->setTrianglesPerBlock(opts.node_faces);

        tree = std::move(treeImpl);
    }

    // Build the hierarchy
    builder.create(tree.get(), stream.get(), opts.top_node_faces);

    // Output handling (.nxs vs .nxz)
    const std::string& finalOutput = output;
    bool hasNxzExtension = endsWithNoCase(finalOutput, ".nxz");
    bool doCompression = hasNxzExtension && opts.enable_compression;

    // The output is written under another name in its folder and given its name only once it
    // is complete: a save that fails (or a process killed while saving) leaves no half file
    // under the output name, and does not remove one an earlier build left there. A deepzoom
    // output is a file and a folder named after it, so it is written in place.
    const std::string writtenOutput = opts.deepzoom ? finalOutput : finalOutput + ".part";

    // If compression is enabled, write a temporary .nxs file first
    const std::string builderOutput = doCompression ? finalOutput + ".tmp.nxs" : writtenOutput;

    writtenOutputGuard.setPath(writtenOutput);
    if (doCompression)
        temporaryNxsGuard.setPath(builderOutput);

    builder.save(builderOutput);

    if (doCompression) {
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
        extractor.save(writtenOutput, signature);
    }

    if (writtenOutput != finalOutput) {
        std::error_code ec;
        std::filesystem::rename(nativePath(writtenOutput), nativePath(finalOutput), ec);
        if (ec)
            throw Error("could not rename " + writtenOutput + " to " + finalOutput + ": " + ec.message());
    }
    writtenOutputGuard.keep();
}

// How many times a build whose nodes turn out to have too many vertices is tried again, each
// time with half as many triangles per node as the time before
constexpr int kSmallerNodeAttempts = 3;

// Builds the Nexus file, falling back to what the input allows; throws on failure. Each attempt
// that fails leaves nothing behind: it fails while building the hierarchy, and its scratch files
// and partial outputs go with it (see buildNexusFile), so the next one starts from scratch.
//  - A node needs a texture too large to pack: the model is built without textures. Models of
//    this kind always built that way.
//  - A node has more vertices than a node holds (a fragmented texture parametrization): the
//    model is built with smaller nodes. Leaving the node out, which Nexus did, leaves a hole in
//    the model.
void buildNexusFileWithFallbacks(const std::string& input,
                                 const std::string& output,
                                 const NexusBuildOptions& opts) {
    NexusBuildOptions attempt = opts;
    int smallerNodeAttempts = 0;
    for (;;) {
        try {
            buildNexusFile(input, output, attempt);
            break;
        } catch (const TexturePackingError& error) {
            if (attempt.disable_texcoords)
                throw;
            std::cerr << "Warning: " << error.what() << std::endl;
            attempt.disable_texcoords = true;
        } catch (const NodeTooLargeError& error) {
            if (smallerNodeAttempts == kSmallerNodeAttempts)
                throw Error(std::string(error.what()) + "; nodes of at most " + std::to_string(attempt.node_faces) +
                            " triangles, the smallest tried (the build started with " +
                            std::to_string(opts.node_faces) + "), are still too large for this model");
            std::cerr << "Warning: " << error.what() << std::endl;
            attempt.node_faces /= 2;
            ++smallerNodeAttempts;
        }
    }

    if (attempt.node_faces != opts.node_faces)
        std::cerr << "Warning: built with nodes of " << attempt.node_faces << " triangles instead of "
                  << opts.node_faces << ": the texture parametrization is too fragmented for larger nodes"
                  << std::endl;
    if (attempt.disable_texcoords && !opts.disable_texcoords)
        std::cerr << "Warning: textures could not be packed; built without textures" << std::endl;
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

    // Inside a try like everything else that can throw: turning a path that is not valid
    // UTF-8 into a file system path throws on Windows, and no exception may leave this C API.
    try {
        if (!pathExists(input)) {
            setErrorMessage(errorMessage,
                            errorMessageSize,
                            std::string("Input file does not exist: ") + input);
            return NXSERR_INVALID_INPUT;
        }
    } catch (const std::exception& e) {
        setErrorMessage(errorMessage, errorMessageSize, std::string("Input path is not valid: ") + e.what());
        return NXSERR_INVALID_INPUT;
    } catch (...) {
        setErrorMessage(errorMessage, errorMessageSize, "Input path is not valid");
        return NXSERR_INVALID_INPUT;
    }

    try {
        // Use "C" numeric locale for this operation only.
        NumericLocaleGuard numericLocaleGuard;

        buildNexusFileWithFallbacks(input, output, opts);

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
