#include "Timers.hpp"

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

namespace nioev {

Timers::~Timers() {
    std::unique_lock<std::mutex> lock{ mTasksMutex };
    mShouldRun = false;
    lock.unlock();
    mTasksCV.notify_all();
    mThread.join();
}
void Timers::tasksThreadFunc() {
    pthread_setname_np(pthread_self(), "script-action-performer");
    while(mShouldRun) {
        std::unique_lock<std::mutex> lock{ mTasksMutex };
        while(mTasks.empty()) {
            mTasksCV.wait(lock);
            if(!mShouldRun) {
                return;
            }
        }
        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration smallestDiff;
        for(auto it = mTasks.begin(); it != mTasks.end(); ++it) {
            std::visit(
                overloaded{ [&](PeriodicTask& p) {
                    auto diff = now - p.lastTimeRun;
                    if((diff) >= p.every) {
                        p.callback();
                        now = std::chrono::steady_clock::now();
                        p.lastTimeRun = now;
                    } else {
                        smallestDiff = std::min(smallestDiff, diff);
                    }
                } },
                *it);
        }
        mTasksCV.wait_for(lock, smallestDiff);
    }
}

}