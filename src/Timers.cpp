#include "Timers.hpp"

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

namespace nioev::mqtt {

Timers::~Timers() {
    std::unique_lock<std::mutex> lock{ mTasksMutex };
    mShouldRun = false;
    mTasksCV.notify_all();
    lock.unlock();
    mThread.join();
}
void Timers::tasksThreadFunc() {
    pthread_setname_np(pthread_self(), "timer");
    while(mShouldRun) {
        std::unique_lock<std::mutex> lock{ mTasksMutex };
        while(mTasks.empty()) {
            mTasksCV.wait(lock);
            if(!mShouldRun) {
                return;
            }
        }
        auto now = std::chrono::steady_clock::now();
        auto smallestDiff = std::chrono::steady_clock::duration::max();
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
        if(smallestDiff == std::chrono::steady_clock::duration::max()) {
            mTasksCV.wait(lock);
        } else {
            mTasksCV.wait_for(lock, smallestDiff);

        }
    }
}

}