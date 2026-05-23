// Named-pipe server.
//
// TODO(phase-3): CreateNamedPipe with PIPE_ACCESS_DUPLEX + overlapped I/O,
// accept loop on a worker thread, dispatch IPC messages from ipc_protocol.h
// against a handler object that controls the pipeline.

#include "ipc_server.h"
#include "shared/log.h"

namespace gpur::daemon {

namespace {
class StubIpcServer final : public IpcServer {
public:
    Result<void> start() override {
        return err(Error::not_implemented("IpcServer (phase 3)"));
    }
    Result<void> stop() override { return ok(); }
};
} // namespace

std::unique_ptr<IpcServer> IpcServer::create() {
    return std::make_unique<StubIpcServer>();
}

} // namespace gpur::daemon
