#include "ScriptContainerManager.hpp"
#include "../Application.hpp"

namespace nioev {

void ScriptContainerManager::deleteScriptFromApp(const std::string& name) {
    mApp.deleteScript(name);
}
}