#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace nioev {

/* This class represents a .so file and wraps in a nice C++ RAII-style. Pretty much every function can throw an exception, so be sure to catch them!
 */
class NativeLibrary final {
public:
    explicit NativeLibrary(std::string path);
    ~NativeLibrary();
    NativeLibrary(const NativeLibrary&) = delete;
    void operator=(const NativeLibrary&) = delete;
    NativeLibrary(NativeLibrary&&) = delete;
    void operator=(NativeLibrary&&) = delete;

    std::vector<std::string> getFeatures();
    void* getFP(const std::string& functionName);
private:
    std::string mPath;
    void* mHandle{ nullptr };
};

}