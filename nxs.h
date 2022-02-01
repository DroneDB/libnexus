#ifndef LIBNEXUS_H


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
    NXSERR_EXCEPTION = 1 // Generic app exception
};

NXS_DLL NXSErr nexusBuild(const char *input, const char *output);

#ifdef __cplusplus
}
#endif

#endif //LIBNEXUS_H