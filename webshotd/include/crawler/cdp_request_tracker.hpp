#pragma once

#include "integers.hpp"
#include "invariant.hpp"
#include "text.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <userver/utils/assert.hpp>

namespace ws::crawler {
using text::literals::operator""_t;

struct [[nodiscard]] CdpPendingRequest final {
    String method;
    std::optional<String> session_id;
    bool ignore_response{false};
};

class [[nodiscard]] CdpRequestTracker final {
public:
    void InsertPending(i64 id, String method, std::optional<String> session_id)
    {
        Insert(
            id, CdpPendingRequest{
                    .method = std::move(method),
                    .session_id = std::move(session_id),
                    .ignore_response = false,
                }
        );
    }

    void InsertIgnored(i64 id, String method, std::optional<String> session_id)
    {
        Insert(
            id, CdpPendingRequest{
                    .method = std::move(method),
                    .session_id = std::move(session_id),
                    .ignore_response = true,
                }
        );
    }

    [[nodiscard]] CdpPendingRequest *Find(i64 id) noexcept
    {
        auto it = requests_.find(id);
        if (it == std::end(requests_))
            return nullptr;
        return &it->second;
    }

    [[nodiscard]] const CdpPendingRequest *Find(i64 id) const noexcept
    {
        auto it = requests_.find(id);
        if (it == std::end(requests_))
            return nullptr;
        return &it->second;
    }

    void MarkIgnoreResponse(i64 id)
    {
        auto *request = Find(id);
        Invariant(request != nullptr, "cannot ignore unknown cdp request"_t);
        request->ignore_response = true;
    }

    void Erase(i64 id) noexcept { requests_.erase(id); }

    void Clear() noexcept { requests_.clear(); }

    [[nodiscard]] size_t Size() const noexcept { return requests_.size(); }

private:
    void Insert(i64 id, CdpPendingRequest request)
    {
        auto [_, inserted] = requests_.emplace(id, std::move(request));
        Invariant(inserted, "duplicate cdp request id"_t);
    }

    std::unordered_map<i64, CdpPendingRequest> requests_;
};

} // namespace ws::crawler
