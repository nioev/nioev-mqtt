#pragma once

#include "../GenServer.hpp"
#include "ScriptContainer.hpp"

namespace nioev {

struct CompileNativeLibraryData {
    ScriptStatusOutput statusOutput;
    std::string codeFilename;
    std::string code;
};

class NativeLibraryManager : public GenServer<CompileNativeLibraryData> {
public:
    NativeLibraryManager();
private:
    void handleTask(CompileNativeLibraryData&&) override;
};

}