#include <fstream>
#include "NativeLibraryManager.hpp"
#include "../Util.hpp"

#include "../quickjs_h_embedded.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <filesystem>

namespace nioev {

NativeLibraryCompiler::NativeLibraryCompiler()
: GenServer<CompileNativeLibraryData>("native- lib-comp") {

}
void NativeLibraryCompiler::handleTask(CompileNativeLibraryData&& nativeLibData) {
    util::DestructWrapper finishLoading{[&] {
        std::unique_lock<std::shared_mutex> lock{mCurrentlyLoadingMutex};
        mCurrentlyLoading.erase(std::string{util::getFileStem(nativeLibData.codeFilename)});
        lock.unlock();
    }};

    std::string path = "/tmp/nioev-temporary-native-code.XXXXXX";
    if(mkdtemp((char*)path.c_str()) == nullptr) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkstemp(): " + util::errnoToString());
        return;
    }
    auto codePath = path + "/" + nativeLibData.codeFilename;
    std::ofstream outCode{codePath};
    outCode << nativeLibData.code;
    outCode.close();
    std::ofstream outQuickJS{path + "/quickjs.h"};
    outQuickJS.write(reinterpret_cast<const char*>(quickjs_h), quickjs_h_len);
    outQuickJS.close();
    if(mkdir("libs", S_IRWXU) < 0 && errno != EEXIST) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkdir(\"libs\"): " + util::errnoToString());
        return;
    }
    std::string baseName{util::getFileStem(nativeLibData.codeFilename)};
    std::string libName = "libs/lib" + baseName + ".so";
    pid_t clangPid = fork();
    if(clangPid < 0) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "fork(): " + util::errnoToString());
        return;
    }
    if(clangPid == 0) {
        // we are clang!
        std::vector<const char*> flags = {"clang", "-fPIC", "-shared", "-g", "-o", libName.c_str(), codePath.c_str()};
        auto firstLine = nativeLibData.code.substr(0, nativeLibData.code.find('\n'));
        if(nativeLibData.code.starts_with("// ")) {
            firstLine = firstLine.substr(3);
            util::splitString(firstLine, ' ', [&](std::string_view part) {
                const_cast<char*>(part.data())[part.size()] = 0; // insert null terminator
                flags.push_back(part.data());
                return util::IterationDecision::Continue;
            });
        }
        flags.push_back(nullptr);
        execvp("clang", const_cast<char**>(flags.data()));
        perror("execlp()");
    }
    int wstatus = 0;
    if(waitpid(clangPid, &wstatus, 0) < 0) {
        kill(clangPid, SIGKILL);
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "mkdir(\"libs\"): " + util::errnoToString());
        return;
    }
    if(!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
        nativeLibData.statusOutput.error(nativeLibData.codeFilename, "Compilation failed!");
        return;
    }
    nativeLibData.statusOutput.success(nativeLibData.codeFilename);
}
void NativeLibraryCompiler::enqueue(CompileNativeLibraryData&& task) {
    std::unique_lock<std::shared_mutex> lock{mCurrentlyLoadingMutex};
    mCurrentlyLoading.emplace(util::getFileStem(task.codeFilename));
    lock.unlock();
    GenServer<CompileNativeLibraryData>::enqueue(std::move(task));
}

}