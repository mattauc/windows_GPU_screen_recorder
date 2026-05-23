#pragma once

#include "shared/ipc_protocol.h"
#include "shared/result.h"

#include <memory>
#include <thread>

namespace gpur::daemon {

// Named-pipe server. Handles connections from gpur-ui.exe.
//
// STATUS: stub. Phase 3.
class IpcServer {
public:
    static std::unique_ptr<IpcServer> create();
    virtual ~IpcServer() = default;

    virtual Result<void> start() = 0;
    virtual Result<void> stop()  = 0;
};

} // namespace gpur::daemon
