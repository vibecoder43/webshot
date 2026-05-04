#pragma once

#include <memory>

namespace ws::crawler {

class [[nodiscard]] BrowserPageSessionLifecycle final {
public:
    BrowserPageSessionLifecycle();
    ~BrowserPageSessionLifecycle();

    BrowserPageSessionLifecycle(const BrowserPageSessionLifecycle &) = delete;
    BrowserPageSessionLifecycle(BrowserPageSessionLifecycle &&) = delete;
    BrowserPageSessionLifecycle &operator=(const BrowserPageSessionLifecycle &) = delete;
    BrowserPageSessionLifecycle &operator=(BrowserPageSessionLifecycle &&) = delete;

    [[nodiscard]] bool MarkBrowserContextCreated();
    [[nodiscard]] bool MarkTargetCreated();
    [[nodiscard]] bool MarkAttached();
    [[nodiscard]] bool MarkBaseDomainsEnabled();
    [[nodiscard]] bool MarkDetached();
    [[nodiscard]] bool MarkDisposed();
    [[nodiscard]] bool MarkClosed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ws::crawler
