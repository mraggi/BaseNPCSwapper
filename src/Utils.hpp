#pragma once

#include "DelayManager.hpp"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BNS::Utils
{

std::string GetFormName(std::uint32_t id);
std::string GetFormName(RE::TESForm* form);
RE::TESForm* GetTrueBaseForm(RE::Actor* a_actor);

void SetActorAngle(RE::Actor* a_actor, const RE::NiPoint3& a_rot);

// FNV-1a 64-bit. Chain by passing the previous return as a_h.
// Pinned algorithm — bumping the record version is the only safe way to
// change this, because rule hashes serialized to saves depend on stable bits.
inline constexpr std::uint64_t kFNV1aBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFNV1aPrime = 0x100000001b3ULL;

inline std::uint64_t FNV1a64(const void* a_data, std::size_t a_size, std::uint64_t a_h = kFNV1aBasis)
{
    const auto* bytes = static_cast<const std::uint8_t*>(a_data);
    for (std::size_t i = 0; i < a_size; ++i)
    {
        a_h ^= bytes[i];
        a_h *= kFNV1aPrime;
    }
    return a_h;
}

// Convenience: chain a trivially-copyable value into the running hash.
template <typename T>
    requires std::is_trivially_copyable_v<T>
inline std::uint64_t FNV1aChain(std::uint64_t a_h, T a_value)
{ return FNV1a64(&a_value, sizeof(T), a_h); }

inline std::uint64_t FNV1aChainStr(std::uint64_t a_h, std::string_view a_s)
{ return FNV1a64(a_s.data(), a_s.size(), a_h); }

// Chains a form's STABLE identifier — (pluginFilename, localFormID) — so
// that rule hashes stored in the co-save remain valid even if the user adds
// mods or reorders the load order. Falls back to the raw runtime FormID only
// if the source plugin can't be found (shouldn't happen post-kGameDataReady).
inline std::uint64_t FNV1aChainFormStable(std::uint64_t a_h, const RE::TESForm* a_form)
{
    if (!a_form) return FNV1aChain(a_h, std::uint32_t {0});

    const std::uint32_t runtimeID = a_form->GetFormID();
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) return FNV1aChain(a_h, runtimeID);

    RE::TESFile* src = nullptr;
    for (auto* f : dh->compiledFileCollection.smallFiles)
        if (f && f->IsFormInMod(runtimeID))
        {
            src = f;
            break;
        }
    if (!src)
        for (auto* f : dh->compiledFileCollection.files)
            if (f && f->IsFormInMod(runtimeID))
            {
                src = f;
                break;
            }

    if (!src) return FNV1aChain(a_h, runtimeID); // fallback: shouldn't happen

    // Local form ID strips the load-order prefix; stable across reorders.
    const std::uint32_t localID = src->IsLight() ? (runtimeID & 0xFFFu) : (runtimeID & 0xFFFFFFu);
    a_h = FNV1aChainStr(a_h, std::string_view {src->filename});
    return FNV1aChain(a_h, localID);
}

// Per-arg conversion: bool stays bool, other integral types → int32_t
// (Papyrus Int is 32-bit signed), everything else passes through. The bool
// branch MUST come before the integral branch — std::is_integral_v<bool>
// is true, and casting bool→int32 trips a Papyrus type-mismatch.
namespace detail
{
    template <typename T>
    constexpr auto ToPapyrusArg(T arg)
    {
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, bool>) { return arg; }
        else if constexpr (std::is_integral_v<D>) { return static_cast<std::int32_t>(arg); }
        else
        {
            return arg;
        }
    }
} // namespace detail

// Calls a static Papyrus function directly. MUST be invoked on the main
// game thread — the Papyrus VM is not thread-safe. Prefer DispatchToPapyrus
// unless you're already inside a TaskInterface callback or VTable hook.
template <typename... Args>
void DispatchToPapyrusAlreadyOnMain(const std::string& a_scriptName, const std::string& a_funcName, Args... a_args)
{
    auto* gameVM = RE::GameVM::GetSingleton();
    auto vm = gameVM ? gameVM->impl.get() : nullptr;
    if (!vm) return;

    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback {nullptr};
    vm->DispatchStaticCall(a_scriptName.c_str(), a_funcName.c_str(), callback, detail::ToPapyrusArg(a_args)...);
}

// Calls a static Papyrus function from any thread. Hops onto the main
// thread via F4SE::GetTaskInterface and then dispatches. No-op if the
// task interface isn't ready yet.
template <typename... Args>
void DispatchToPapyrus(const std::string& a_scriptName, const std::string& a_funcName, Args... a_args)
{
    auto* task = F4SE::GetTaskInterface();
    if (!task) return;
    task->AddTask(
      [a_scriptName, a_funcName, a_args...]() { DispatchToPapyrusAlreadyOnMain(a_scriptName, a_funcName, a_args...); });
}

// ---------------------------------------------------------------------------
// AsyncTokenRegistry
// ---------------------------------------------------------------------------
// One-shot, key-and-token-based callback registry. Every Park() returns a
// unique opaque Token. The Papyrus-callback side calls `ConsumeOldest(key)`
// which pops the oldest entry for that key (FIFO order — matches the order
// dispatches were made). The timeout side calls `ConsumeToken(token)` which
// only fires its own entry; if Papyrus has already consumed it, the timeout
// is a no-op.
//
// This buys us two things that the previous single-slot map didn't:
//   (a) Two Park()s for the same key no longer overwrite each other — they
//       queue, and the matching number of Consumes drains them in order.
//   (b) A late Papyrus callback after its timeout fired pops the NEXT
//       parked entry (or nothing) instead of double-firing the same one.
//
// Used by DispatchToPapyrusAwait below.
template <typename Key>
class AsyncTokenRegistry
{
public:
    using Callback = std::function<void()>;
    using Token = std::uint64_t;

    Token Park(Key a_key, Callback a_cb)
    {
        const Token token = m_nextToken.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_map[a_key].push_back(Entry {token, std::move(a_cb)});
        return token;
    }

    // FIFO: Papyrus callback consumes the oldest park for this key.
    Callback ConsumeOldest(Key a_key)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_map.find(a_key);
        if (it == m_map.end() || it->second.empty()) return {};
        Callback cb = std::move(it->second.front().cb);
        it->second.erase(it->second.begin());
        if (it->second.empty()) m_map.erase(it);
        return cb;
    }

    // Targeted: timeout consumes its own token only. No-op if Papyrus
    // already consumed it via ConsumeOldest.
    Callback ConsumeToken(Token a_token)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_map.begin(); it != m_map.end(); ++it)
        {
            auto& vec = it->second;
            for (auto vit = vec.begin(); vit != vec.end(); ++vit)
            {
                if (vit->token == a_token)
                {
                    Callback cb = std::move(vit->cb);
                    vec.erase(vit);
                    if (vec.empty()) m_map.erase(it);
                    return cb;
                }
            }
        }
        return {};
    }

private:
    struct Entry
    {
        Token token;
        Callback cb;
    };
    mutable std::mutex m_mutex;
    std::unordered_map<Key, std::vector<Entry>> m_map;
    std::atomic<Token> m_nextToken {1};
};

// ---------------------------------------------------------------------------
// DispatchToPapyrusAwait
// ---------------------------------------------------------------------------
// Park a resume + fire a Papyrus call + arm a timeout, all in one. The
// `a_onResume` callback fires EXACTLY ONCE — whichever path arrives first
// (Papyrus callback via the registry's ConsumeOldest, or timeout via
// ConsumeToken). Safe to call from any thread: the dispatch hops to main.
//
// Caller is responsible for binding the script-side callback to call
// `a_registry.ConsumeOldest(key)` when it finishes (and invoking the
// returned Callback if any).
template <typename Key, typename... Args>
void DispatchToPapyrusAwait(AsyncTokenRegistry<Key>& a_registry,
                            Key a_key,
                            int a_timeoutMs,
                            std::function<void()> a_onResume,
                            const std::string& a_scriptName,
                            const std::string& a_funcName,
                            Args... a_args)
{
    const auto token = a_registry.Park(a_key, std::move(a_onResume));
    DispatchToPapyrus(a_scriptName, a_funcName, a_args...);
    DelayManager::QueueTask(a_timeoutMs, [&a_registry, token]() {
        if (auto cb = a_registry.ConsumeToken(token); cb) cb();
    });
}

} // namespace BNS::Utils
