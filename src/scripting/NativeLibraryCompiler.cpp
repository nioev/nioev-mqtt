#include <fstream>
#include "NativeLibraryCompiler.hpp"
#include "nioev/lib/Util.hpp"

#include "../quickjs_h_embedded.hpp"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <filesystem>

namespace nioev {

using namespace nioev::lib;

NativeLibraryCompiler::NativeLibraryCompiler()
: GenServer<CompileNativeLibraryData>("native-lib-comp") {
    startThread();
}
void NativeLibraryCompiler::handleTask(CompileNativeLibraryData&& nativeLibData) {
    DestructWrapper finishLoading{[&] {
        std::unique_lock<std::shared_mutex> lock{mCurrentlyLoadingMutex};
        mCurrentlyLoading.erase(std::string{getFileStem(nativeLibData.codeFilename)});
        lock.unlock();
    }};

    std::string path = "/tmp/nioev-temporary-native-code.XXXXXX";
    if(mkdtemp((char*)path.c_str()) == nullptr) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkstemp(): " + errnoToString());
        return;
    }
    DestructWrapper deleteTempDir{[&] {
        std::filesystem::remove_all(path);
    }};
    auto codePath = path + "/" + nativeLibData.codeFilename;
    std::ofstream outCode{codePath};
    outCode << nativeLibData.code;
    outCode.close();
    std::ofstream outQuickJS{path + "/quickjs.h"};
    outQuickJS.write(reinterpret_cast<const char*>(quickjs_h), quickjs_h_len);
    outQuickJS.close();
    if(mkdir("libs", S_IRWXU) < 0 && errno != EEXIST) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkdir(\"libs\"): " + errnoToString());
        return;
    }
    std::string baseName{getFileStem(nativeLibData.codeFilename)};
    std::string libName = "libs/lib" + baseName + ".so";
    pid_t clangPid = fork();
    if(clangPid < 0) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "fork(): " + errnoToString());
        return;
    }
    if(clangPid == 0) {
        // we are clang!
        std::vector<const char*> flags = {"g++", "-std=c++17", "-lstdc++", "-fPIC", "-shared", "-g", "-o", libName.c_str(), codePath.c_str()};
        auto firstLine = nativeLibData.code.substr(0, nativeLibData.code.find('\n'));
        if(nativeLibData.code.starts_with("// ")) {
            firstLine = firstLine.substr(3);
            splitString(firstLine, ' ', [&](std::string_view part) {
                const_cast<char*>(part.data())[part.size()] = 0; // insert null terminator
                flags.push_back(part.data());
                return IterationDecision::Continue;
            });
        }
        flags.push_back(nullptr);
        execvp("g++", const_cast<char**>(flags.data()));
        perror("execlp()");
    }
    int wstatus = 0;
    if(waitpid(clangPid, &wstatus, 0) < 0) {
        kill(clangPid, SIGKILL);
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkdir(\"libs\"): " + errnoToString());
        return;
    }
    if(!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "Compilation failed!");
        return;
    }
    nativeLibData.statusOutput.success(nativeLibData.codeFilename, "");
}
void NativeLibraryCompiler::enqueue(CompileNativeLibraryData&& task) {
    std::unique_lock<std::shared_mutex> lock{mCurrentlyLoadingMutex};
    mCurrentlyLoading.emplace(getFileStem(task.codeFilename));
    lock.unlock();
    GenServer<CompileNativeLibraryData>::enqueue(std::move(task));
}

}