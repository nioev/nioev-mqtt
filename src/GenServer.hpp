#pragma once
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cassert>
#include <optional>

namespace nioev {

/* A class that represents a similar concept to that of a GenServer in elixir - that's where the name comes frome. You put a request and it while get
 * handled by a second worker thread. This is a pattern that's used quite a lot and is very useful.
 */
template<typename TaskType>
class GenServer {
public:
    explicit GenServer(const char* threadName)
    : mThreadName(threadName) { }

    virtual ~GenServer() {
        std::unique_lock<std::mutex> lock{ mTasksMutex };
        mShouldRun = false;
        mTasksCV.notify_all();
        lock.unlock();
        mWorkerThread->join();
    }
    virtual void enqueue(TaskType&& task) {
        assert(mWorkerThread);
        std::unique_lock<std::mutex> lock{mTasksMutex};
        mTasks.emplace(std::move(task));
        mTasksCV.notify_all();
    }

protected:
    void startThread() {
        mWorkerThread.template emplace([this]{workerThreadFunc();});
    }
    virtual void handleTask(TaskType&&) = 0;
private:
    void workerThreadFunc() {
        pthread_setname_np(pthread_self(), mThreadName);
        std::unique_lock<std::mutex> lock{mTasksMutex};
        while(true) {
            mTasksCV.wait(lock);
            if(!mShouldRun)
                return;
            while(!mTasks.empty()) {
                auto pub = std::move(mTasks.front());
                lock.unlock();
                handleTask(std::move(pub));
                lock.lock();
                mTasks.pop();
            }

        }
    }
    bool mShouldRun{true};
    std::mutex mTasksMutex;
    std::condition_variable mTasksCV;
    std::queue<TaskType> mTasks;
    const char* mThreadName;
    std::optional<std::thread> mWorkerThread;
};

}