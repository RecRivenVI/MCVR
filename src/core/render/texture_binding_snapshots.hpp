#pragma once

#include <map>
#include <memory>
#include <stdexcept>

namespace mcvr {
// CPU bindings may change while earlier commands still use an acquired immutable snapshot.
// The renderer retains each acquired snapshot until the corresponding frame fence retires.
template<class Binding, class Snapshot>
class TextureBindingSnapshots {
public:
    bool bind(unsigned slot, Binding binding) {
        if (slot >= 4096) throw std::out_of_range("Texture descriptor slot");
        auto it = bindings_.find(slot);
        if (it != bindings_.end() && it->second == binding) return false;
        bindings_.insert_or_assign(slot, std::move(binding));
        dirty_ = true;
        return true;
    }
    template<class Factory>
    std::shared_ptr<Snapshot> acquire(Factory &&factory) {
        if (dirty_ || !snapshot_) {
            auto next = factory(bindings_);
            snapshot_ = std::move(next);
            dirty_ = false;
        }
        return snapshot_;
    }
private:
    std::map<unsigned, Binding> bindings_;
    std::shared_ptr<Snapshot> snapshot_;
    bool dirty_ = true;
};
}
