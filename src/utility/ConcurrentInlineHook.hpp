#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <safetyhook.hpp>

namespace utility {
// An inline hook whose original can be called from many threads at once.
// SafetyHook's InlineHook::call() holds the hook's mutex for the whole original call, so every
// caller of a hot function (ID3D12GraphicsCommandList::ResourceBarrier on every render thread)
// queues behind one lock. Here the hook is published once and never replaced; callers take a
// lease that keeps the trampoline alive while they are inside the original.
// Ported from satyaloka93/UEVR-AFW-Compat (utility::ConcurrentInlineHook).
class ConcurrentInlineHook {
public:
    using Lease = std::shared_ptr<const safetyhook::InlineHook>;

    ~ConcurrentInlineHook() {
        if (auto hook = m_hook.load()) {
            // Disable before dropping ownership; live leases keep the trampoline allocated
            // until their original calls return.
            (void)hook->disable();
        }
    }

    std::optional<std::string> install(void* target, void* detour) {
        std::scoped_lock lock{m_install_mutex};

        if (m_hook.load()) {
            return "hook already installed; live replacement is unsupported";
        }

        auto result = safetyhook::InlineHook::create(target, detour, safetyhook::InlineHook::StartDisabled);

        if (!result) {
            return "create failed: " + std::to_string(static_cast<int>(result.error().type));
        }

        auto hook = std::make_shared<safetyhook::InlineHook>(std::move(*result));

        // A thread can enter the detour as soon as enable() patches the target, so the
        // complete hook must be visible to acquire() before that.
        m_hook.store(hook, std::memory_order_release);

        if (auto enabled = hook->enable(); !enabled) {
            return "enable failed: " + std::to_string(static_cast<int>(enabled.error().type));
        }

        return std::nullopt;
    }

    [[nodiscard]] Lease acquire() const {
        return m_hook.load(std::memory_order_acquire);
    }

private:
    std::mutex m_install_mutex;
    std::atomic<std::shared_ptr<safetyhook::InlineHook>> m_hook{};
};
}
