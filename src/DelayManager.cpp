#include "DelayManager.hpp"
#include <F4SE/F4SE.h>

namespace BNS
{

constexpr int kWaitForMs = 48;

DelayManager& DelayManager::GetSingleton()
{
    static DelayManager instance;
    return instance;
}

void DelayManager::QueueTask(int delayMS, std::function<void()> task)
{
    auto& s = GetSingleton();
    const auto executeTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMS);
    std::lock_guard<std::mutex> lock(s.m_mutex);
    s.m_tasks.push_back({executeTime, std::move(task)});
}

void DelayManager::QueuePoller(uint32_t formID,
                               int delayMS,
                               int maxAttempts,
                               std::function<bool(RE::Actor*, int)> predicate,
                               std::function<void(RE::Actor*, int)> onSuccess,
                               std::function<void(RE::Actor*)> onTimeout,
                               std::function<void(uint32_t)> onFail,
                               int currentAttempt)
{
    // Lambdas call back into the public static API, no 'this' needed.
    QueueTask(delayMS, [=]() {
        F4SE::GetTaskInterface()->AddTask([=]() {
            auto* form = RE::TESForm::GetFormByID(formID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor)
            {
                if (onFail) onFail(formID);
                return;
            }

            if (predicate(actor, currentAttempt))
            {
                if (onSuccess) onSuccess(actor, currentAttempt);
            }
            else if (currentAttempt < maxAttempts)
            {
                QueuePoller(formID, delayMS, maxAttempts, predicate, onSuccess, onTimeout, onFail, currentAttempt + 1);
            }
            else
            {
                if (onTimeout) onTimeout(actor);
            }
        });
    });
}

void DelayManager::Start()
{
    auto& s = GetSingleton();
    // CAS so concurrent double-Start can't spawn two workers.
    bool expected = false;
    if (!s.m_running.compare_exchange_strong(expected, true)) return;

    // Capture reference to the static singleton — safe, it lives forever.
    s.m_worker = std::thread([&s]() {
        while (s.m_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kWaitForMs));
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::function<void()>> readyTasks;
            {
                std::lock_guard<std::mutex> lock(s.m_mutex);
                auto it = s.m_tasks.begin();
                while (it != s.m_tasks.end())
                {
                    if (now >= it->executeTime)
                    {
                        readyTasks.push_back(std::move(it->func));
                        it = s.m_tasks.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (auto& task : readyTasks)
                task();
        }
    });
    s.m_worker.detach();
}

} // namespace BNS
