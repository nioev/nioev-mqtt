#pragma once

#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <variant>

namespace nioev::mqtt {

class Timers final {
public:
    struct PeriodicTask {
        std::chrono::steady_clock::duration every;
        std::function<void()> callback;
        std::chrono::steady_clock::time_point lastTimeRun;
    };

    Timers() : mThread([this] { tasksThreadFunc(); }) { }
    ~Timers();

    void tasksThreadFunc();

    void addPeriodicTask(std::chrono::steady_clock::duration every, std::function<void()>&& callback) {
        std::lock_guard<std::mutex> lock{ mTasksMutex };
        mTasks.emplace_back(PeriodicTask{every, std::move(callback), std::chrono::steady_clock::now()});
        mTasksCV.notify_one();
    }

private:
    std::atomic<bool> mShouldRun = true;
    std::mutex mTasksMutex;
    std::condition_variable mTasksCV;
    std::list<std::variant<PeriodicTask>> mTasks;
    std::thread mThread;
};

}