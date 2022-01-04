#include "NativeLibrary.hpp"
#include <dlfcn.h>

namespace nioev {

NativeLibrary::NativeLibrary(std::string path)
: mPath(std::move(path)) {
    mHandle = dlopen(mPath.c_str(), RTLD_LAZY);
    if(mHandle == nullptr) {
        throw std::runtime_error{dlerror()};
    }
}
NativeLibrary::~NativeLibrary() {
    dlclose(mHandle);
}
std::vector<std::string> NativeLibrary::getFeatures() {
    auto fp = (const char**(*)())dlsym(mHandle, "_nioev_library_function_get_features");
    if(!fp) {
        throw std::runtime_error{dlerror()};
    }
    auto features = fp();
    if(features == nullptr) {
        throw std::runtime_error{"Dynamic library returned nullptr for features list!"};
    }
    std::vector<std::string> ret;
    for(int i = 0; features[i] != nullptr; ++i) {
        ret.emplace_back(features[i]);
    }
    return ret;
}
void* NativeLibrary::getFP(const std::string& functionName) {
    auto ptr = dlsym(mHandle, functionName.c_str());
    if(ptr == nullptr) {
        throw std::runtime_error{"Native function " + functionName + " doesn't exist: " + dlerror()};
    }
    return ptr;
}
}