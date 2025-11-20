#ifndef LIBNEXUS_H

#include <string>

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
    #define NXS_DLL   __declspec(dllexport)
#else
    #define NXS_DLL
#endif // _WIN32

enum NXSErr {
    NXSERR_NONE = 0, // No error
    NXSERR_EXCEPTION = 1, // Generic app exception
    NXSERR_INVALID_INPUT = 2 // Invalid input
};

struct NexusBuildOptions {
    // --- Geometry build options ---
    int   node_faces            = 1 << 14;  // triangles per node = 16384
    float texel_weight          = 0.1f;     // relative weight of texels
    int   top_node_faces        = 4096;
    float vertex_quantization   = 0.0f;     // 0.0 = disabled
    int   texture_quality       = 95;       // JPEG texture quality
    int   ram_buffer_mb         = 16000;    // approximate RAM budget (MB)
    float scaling               = 0.5f;     // simplification ratio
    int   skip_levels           = 0;        // skip simplify levels
    float adaptive              = 0.333f;   // KDTree adaptive factor

    bool  point_cloud           = false;
    bool  force_normals         = false;
    bool  disable_normals       = false;
    bool  force_colors          = false;
    bool  disable_colors        = false;
    bool  disable_texcoords     = false;
    bool  use_original_textures = false;
    bool  create_pow_two_textures = false;
    bool  deepzoom              = false;

    // --- Compression options (used when output extension is .nxz) ---
    // If enable_compression is false, no compression is performed even
    // if the output file has .nxz extension.
    bool        enable_compression = true;
    std::string compress_lib       = "corto"; // "corto" or "meco"

    // Quantization controls:
    // - if coord_step  > 0.0: use that global precision
    // - else if position_bits > 0: derive coord_step from bounding sphere
    // - else if error_q > 0.0: derive coord_step from node error
    float coord_step    = 0.0f;
    int   position_bits = 0;
    float error_q       = 0.1f;

    int   luma_bits     = 6;
    int   chroma_bits   = 6;
    int   alpha_bits    = 5;
    int   normal_bits   = 10;
    float tex_step      = 0.25f;   // in pixels
};

NXS_DLL NXSErr nexusBuild(const char *input,
                          const char *output,
                          char *errorMessage = nullptr,
                          int errorMessageSize = 0);

NXS_DLL NXSErr nexusBuildEx(const char* input,
                            const char* output,
                            const NexusBuildOptions& options,
                            char* errorMessage,
                            int errorMessageSize);

#ifdef __cplusplus
}
#endif

#endif //LIBNEXUS_H