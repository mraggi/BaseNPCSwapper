#pragma once

#include <RE/Fallout.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace BNS
{

class DelayManager
{
public:
    static void Start();

    static void QueueTask(int delayMS, std::function<void()> task);

    static void QueuePoller(uint32_t formID,
                            int delayMS,
                            int maxAttempts,
                            std::function<bool(RE::Actor*, int)> predicate,
                            std::function<void(RE::Actor*, int)> onSuccess,
                            std::function<void(RE::Actor*)> onTimeout,
                            std::function<void(uint32_t)> onFail,
                            int currentAttempt = 1);

private:
    static DelayManager& GetSingleton();

    struct DelayedTask
    {
        std::chrono::steady_clock::time_point executeTime;
        std::function<void()> func;
    };

    std::vector<DelayedTask> m_tasks;
    std::mutex m_mutex;
    std::thread m_worker;
    std::atomic<bool> m_running {false};
};

} // namespace BNS
