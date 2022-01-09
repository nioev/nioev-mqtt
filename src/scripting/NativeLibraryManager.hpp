#pragma once

#include "../GenServer.hpp"
#include "ScriptContainer.hpp"
#include <unordered_set>
#include <shared_mutex>

namespace nioev {

struct CompileNativeLibraryData {
    ScriptStatusOutput statusOutput;
    std::string codeFilename;
    std::string code;
};

class NativeLibraryManager : public GenServer<CompileNativeLibraryData> {
public:
    NativeLibraryManager();
    std::pair<std::reference_wrapper<const std::unordered_set<std::string>>, std::shared_lock<std::shared_mutex>> getListOfCurrentlyLoadingNativeLibs() const {
        std::shared_lock<std::shared_mutex> lock{mCurrentlyLoadingMutex};
        return {mCurrentlyLoading, std::move(lock)};
    }

    void enqueue(CompileNativeLibraryData&& task) override;
private:
    void handleTask(CompileNativeLibraryData&&) override;

    // This map allows scripts to stall their execution while the native lib is (re-)compiling
    std::unordered_set<std::string> mCurrentlyLoading;
    mutable std::shared_mutex mCurrentlyLoadingMutex;
};

}