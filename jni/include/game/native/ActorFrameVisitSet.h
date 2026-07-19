#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace lengjing::game::native {

class ActorFrameVisitSet final {
public:
    void Reserve(std::size_t count) {
        actors_.reserve(count);
    }

    bool TryVisit(std::uintptr_t actor) {
        return actor != 0 && actors_.insert(actor).second;
    }

    std::size_t Size() const noexcept {
        return actors_.size();
    }

private:
    std::unordered_set<std::uintptr_t> actors_;
};

}  // namespace lengjing::game::native
