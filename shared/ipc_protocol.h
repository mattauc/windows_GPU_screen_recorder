#pragma once

// Wire protocol between gpur-daemon.exe and gpur-ui.exe over a named pipe.
//
// Each message is length-prefixed:
//   uint32_t  length_le        // little-endian, length of payload that follows
//   uint8_t   payload[length]  // MessagePack-encoded MessageEnvelope
//
// Phase 3 work — daemon only listens; UI client is implemented when the WinUI
// app lands. For now this header just pins the wire-level shape so we don't
// paint ourselves into a corner.

#include <cstdint>
#include <string>
#include <vector>

namespace gpur::ipc {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\gpur-daemon";

enum class MessageType : uint16_t {
    // Client → Daemon
    Ping                = 0x0001,
    StartRecording      = 0x0010,
    StopRecording       = 0x0011,
    SaveReplay          = 0x0012,
    GetStatus           = 0x0020,
    GetSettings         = 0x0021,
    SetSettings         = 0x0022,

    // Daemon → Client
    Pong                = 0x8001,
    RecordingStarted    = 0x8010,
    RecordingStopped    = 0x8011,
    ReplaySaved         = 0x8012,
    Status              = 0x8020,
    Settings            = 0x8021,
    Error               = 0x80FF,
};

struct MessageHeader {
    uint16_t version{1};
    MessageType type{};
    uint32_t correlation_id{};
};

// Body payloads are kept loose for now. Each message type has its own POD-like
// struct that MessagePack serialises. Defined alongside ipc_server.cpp once we
// implement Phase 3.

} // namespace gpur::ipc
