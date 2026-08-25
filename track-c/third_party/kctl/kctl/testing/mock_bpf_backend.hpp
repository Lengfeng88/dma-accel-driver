#pragma once
#include "kctl/bpf_skeleton_guard.hpp"

namespace kctl::testing {

// Opaque fake skeleton type -- real usage would be a libbpf-generated struct.
struct FakeSkel {
    int id = 0;
};

class MockBpfBackend : public IBpfBackend<FakeSkel> {
public:
    enum class FailAt { None, Open, Load, Attach };

    explicit MockBpfBackend(FailAt fail_at = FailAt::None) : fail_at_(fail_at) {}

    FakeSkel* open() override {
        ++open_calls;
        if (fail_at_ == FailAt::Open) return nullptr;
        skel_.id = 1;
        return &skel_;
    }

    int load(FakeSkel*) override {
        ++load_calls;
        return fail_at_ == FailAt::Load ? -1 : 0;
    }

    int attach(FakeSkel*) override {
        ++attach_calls;
        return fail_at_ == FailAt::Attach ? -1 : 0;
    }

    void detach(FakeSkel*) override { ++detach_calls; }
    void destroy(FakeSkel*) override { ++destroy_calls; }

    int open_calls = 0;
    int load_calls = 0;
    int attach_calls = 0;
    int detach_calls = 0;
    int destroy_calls = 0;

private:
    FailAt fail_at_;
    FakeSkel skel_;
};

} // namespace kctl::testing
