#include "nxs.h"

#include <locale.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QStringList>
#include <QtPlugin>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#ifdef WIN32
#include <windows.h>
#endif

#include <wrap/system/qgetopt.h>

#include "../common/qtnexusfile.h"
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
    std::strncpy(buffer, message.c_str(), bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
}

void setErrorMessage(char* buffer, int bufferSize, const QString& message) {
    if (!buffer || bufferSize <= 0)
        return;
    QByteArray ba = message.toLatin1();
    std::strncpy(buffer, ba.constData(), bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
}

void setErrorMessage(char* buffer, int bufferSize, const char* message) {
    if (!buffer || bufferSize <= 0)
        return;
    std::strncpy(buffer, message, bufferSize - 1);
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
    explicit TempFileGuard(const QString& path) : path_(path) {}

    void setPath(const QString& path) { path_ = path; }
    const QString& path() const { return path_; }

    ~TempFileGuard() {
        if (!path_.isEmpty()) {
            QFile::remove(path_);
        }
    }

private:
    QString path_;
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

            // Static instance, lives for the whole process lifetime.
            static QCoreApplication app(argc, argv);
            Q_UNUSED(app);
        }

#ifdef WIN32
        // On Windows, derive plugin path from this DLL location.
        HMODULE hModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&initializeQtEnvironment),
                               &hModule)) {
            char dllPath[MAX_PATH] = {};
            if (GetModuleFileNameA(hModule, dllPath, MAX_PATH) > 0) {
                QFileInfo dllInfo(QString::fromLocal8Bit(dllPath));
                QString pluginPath = dllInfo.absolutePath() + "/plugins";

                if (QDir(pluginPath).exists()) {
                    QCoreApplication::addLibraryPath(pluginPath);
#if defined(DEBUG)
                    qDebug() << "Added Qt plugin path: " << pluginPath;
                    qDebug() << "Qt library paths: " << QCoreApplication::libraryPaths();
#endif
                }
            }
        }
#else
        // On Unix-like systems, fall back to the application directory.
        QString appPath    = QCoreApplication::applicationDirPath();
        QString pluginPath = appPath + "/plugins";
        if (QDir(pluginPath).exists()) {
            QCoreApplication::addLibraryPath(pluginPath);
        }
#endif
    });
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

    QFile inputFile(QString::fromUtf8(input));
    if (!inputFile.exists()) {
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
        qDebug() << "Supported image formats: " << QImageReader::supportedImageFormats();
#endif

        const quint64 max_memory =
            (quint64(1) << 20) * static_cast<quint64>(opts.ram_buffer_mb) / 4;

        QStringList inputs;
        inputs.append(QString::fromUtf8(input));

        vcg::Point3d origin(0, 0, 0);

        std::unique_ptr<Stream> stream;
        std::unique_ptr<KDTree> tree;

        // Build a somewhat unique base name for cache files
        using Clock = std::chrono::steady_clock;
        auto now = Clock::now().time_since_epoch().count();
        std::string cacheBase =
            "nexus_cache_" + std::to_string(static_cast<unsigned long long>(now));

        // Create and load mesh stream
        {
            auto streamImpl = std::make_unique<StreamSoup>((cacheBase + "_stream").c_str());
            streamImpl->setVertexQuantization(opts.vertex_quantization);
            streamImpl->setMaxMemory(max_memory);
            streamImpl->origin = origin;

            QString mtl;  // not exposed yet
            streamImpl->load(inputs, mtl);

            stream = std::move(streamImpl);
        }

        const bool has_colors = stream->hasColors();
        const bool has_normals = stream->hasNormals();
        const bool has_textures = stream->hasTextures();

        quint32 components = 0;
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

        NexusBuilder builder(components);
        builder.skipSimplifyLevels = opts.skip_levels;
        builder.setMaxMemory(max_memory);

        int n_threads = static_cast<int>(std::thread::hardware_concurrency());
        builder.n_threads = n_threads == 0 ? 1 : n_threads;

        builder.setScaling(opts.scaling);
        builder.useNodeTex = !opts.use_original_textures;
        builder.createPowTwoTex = opts.create_pow_two_textures;

        if (opts.deepzoom) {
            builder.header.signature.flags |= nx::Signature::Flags::DEEPZOOM;
        }

        builder.tex_quality = opts.texture_quality;

        if (!builder.initAtlas(stream->textures)) {
            setErrorMessage(errorMessage, errorMessageSize, "Failed to initialize texture atlas");
            return NXSERR_EXCEPTION;
        }

        // Create KD-tree
        {
            auto treeImpl =
                std::make_unique<KDTreeSoup>((cacheBase + "_tree").c_str(), opts.adaptive);
            treeImpl->setMaxMemory((quint64(1) << 20) * static_cast<quint64>(opts.ram_buffer_mb) /
                                   2);

            treeImpl->setMaxWeight(opts.node_faces);
            treeImpl->texelWeight = opts.texel_weight;
            treeImpl->setTrianglesPerBlock(opts.node_faces);

            tree = std::move(treeImpl);
        }

        // Build the hierarchy
        builder.create(tree.get(), stream.get(), opts.top_node_faces);

        // Output handling (.nxs vs .nxz)
        QString finalOutput = QString::fromUtf8(output);
        bool hasNxzExtension = finalOutput.endsWith(".nxz", Qt::CaseInsensitive);
        bool doCompression = hasNxzExtension && opts.enable_compression;

        QString builderOutput = finalOutput;
        if (doCompression) {
            // If compression is enabled, write a temporary .nxs file first
            builderOutput += ".tmp.nxs";
        }

        builder.save(builderOutput);

        if (doCompression) {
            // Ensure temporary .nxs is always removed
            TempFileGuard tempNxsGuard(builderOutput);

            QStringList nexusInputs;
            nexusInputs.append(builderOutput);

            NexusData nexus;
            nexus.file = new QTNexusFile();

            if (!nexus.open(nexusInputs[0].toLatin1())) {
                setErrorMessage(
                    errorMessage,
                    errorMessageSize,
                    std::string("Could not open file: ") + nexusInputs[0].toStdString());
                return NXSERR_EXCEPTION;
            }

            // Validate compression library
            Signature signature = nexus.header.signature;
            signature.flags &= ~(Signature::MECO | Signature::CORTO);

            if (opts.compress_lib == "meco") {
                signature.flags |= Signature::MECO;
            } else if (opts.compress_lib == "corto") {
                signature.flags |= Signature::CORTO;
            } else {
                setErrorMessage(errorMessage,
                                errorMessageSize,
                                std::string("Unknown compression method: ") + opts.compress_lib);
                return NXSERR_EXCEPTION;
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

        return NXSERR_NONE;
    } catch (const QString& error) {
        std::cerr << "Fatal error (QString): " << error.toStdString() << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, error);
        return NXSERR_EXCEPTION;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error (std::exception): " << e.what() << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, std::string("Exception: ") + e.what());
        return NXSERR_EXCEPTION;
    } catch (const char* error) {
        std::cerr << "Fatal error (const char*): " << error << std::endl;
        setErrorMessage(errorMessage, errorMessageSize, std::string(error));
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
