#pragma once

#include <variant>
#include <string>
#include "../Forward.hpp"

struct ScriptActionSubscribe {
    std::string scriptName;
    std::string topic;
};
struct ScriptActionUnsubscribe {
    std::string scriptName;
    std::string topic;
};

struct ScriptActionPublish {

};

using ScriptAction = std::variant<ScriptActionPublish, ScriptActionSubscribe, ScriptActionUnsubscribe>;

namespace nioev {

class ScriptActionPerformer {
public:
    explicit ScriptActionPerformer(Application& app);
    void enqueueAction(ScriptAction&&);
private:
    Application& mApp;
};

}
