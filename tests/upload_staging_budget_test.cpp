#include "core/render/upload_staging_budget.hpp"

#include <cassert>

int main() {
    assert(mcvr::render::replacementUploadCapacity(0, 16) == 16);
    assert(mcvr::render::replacementUploadCapacity(4, 16) == 16);
    assert(mcvr::render::replacementUploadCapacity(48, 16) == 48);

    mcvr::render::UploadStagingBudget budget(64);
    assert(budget.tryRetain(48));
    assert(!budget.tryRetain(17));
    assert(budget.tryRetain(16));
    assert(budget.retainedBytes() == 64);
    budget.take(48);
    assert(budget.retainedBytes() == 16);
    assert(budget.tryRetain(48));
    assert(!budget.tryRetain(65));
    budget.take(128);
    assert(budget.retainedBytes() == 0);
}
