#ifndef BACKENDS_XAUDIO2_BACKEND_H
#define BACKENDS_XAUDIO2_BACKEND_H

/* Not "xaudio2.h": that is the Windows SDK's header. */
#include "backends/base.h"

struct XAudio2BackendFactory final : public BackendFactory {
public:
    auto init() -> bool final;

    auto querySupport(BackendType type) -> bool final;

    auto enumerate(BackendType type) -> std::vector<std::string> final;

    auto createBackend(DeviceBase *device, BackendType type) -> BackendPtr final;

    static auto getFactory() -> BackendFactory&;
};

#endif /* BACKENDS_XAUDIO2_BACKEND_H */
