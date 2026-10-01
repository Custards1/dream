// SHA-256, for `io.digest` and `io.digest!`.
//
// Written out rather than taken from a library because the VM's only required
// dependency is libffi, and a build cache's keys are not a reason to add a
// second. SHA-256 rather than something faster because the same digest is
// what pins a fetched source (`std.build.fetch`), and there the number has to
// be the one the source's publisher printed -- which is always this one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dream {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    /// The digest as 64 lowercase hex digits. The object is spent afterwards.
    std::string hex();

private:
    void block(const uint8_t* p);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

}  // namespace dream
