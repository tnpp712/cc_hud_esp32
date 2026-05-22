// ota_server.h
// BLE-driven OTA firmware updater for cc_hud.
//
// Adds a second GATT service onto the existing NimBLE server (the one
// `bleServerInit` brought up) with three characteristics:
//
//   * Control  (write+response)    — 1-byte command + optional payload
//   * Data     (write+writeNoResp) — raw firmware bytes, streamed
//   * State    (read+notify)       — short ASCII status feedback
//
// Control commands:
//   0x00 START + u32 LE total_size   start a new OTA, allocate inactive slot
//   0x01 END                         finalise + reboot into the new image
//   0x02 ABORT                       discard in-progress write
//
// State strings the client may see:
//   "IDLE"            initial value (no OTA in progress)
//   "READY"           START accepted, ready for Data chunks
//   "PROG <bytes>"    progress notification (rate-limited ~8 KB)
//   "OK"              END succeeded, device is about to reboot
//   "ABORTED"         ABORT processed cleanly
//   "ERR <reason>"    various failure modes (begin/write/end/cmd/len)
//
// Usage:
//   bleServerInit(...);                  // starts the BLE stack
//   otaServerInit(bleGetServer());       // adds the OTA service

#pragma once

namespace cc_hud {

}  // namespace cc_hud

// Forward declaration matching NimBLE's header.
class NimBLEServer;

namespace cc_hud {

// Register the OTA GATT service on `server`. Safe to call once after the BLE
// stack has been initialised. No-op if `server` is null.
void otaServerInit(NimBLEServer* server);

}  // namespace cc_hud
