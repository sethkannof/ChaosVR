#pragma once
#include <stdexcept>
#include <string>
#include <openxr/openxr.h>

namespace chaosvr::host {
inline const char* XrResultName(XrResult r) {
    switch (r) {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
#ifdef XR_ERROR_RUNTIME_UNAVAILABLE
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
#endif
    default: return "XR_ERROR_UNKNOWN";
    }
}
inline void XrCheck(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
        throw std::runtime_error(std::string(what) + " failed: " + XrResultName(r) +
            " (" + std::to_string((int)r) + ")");
    }
}
}