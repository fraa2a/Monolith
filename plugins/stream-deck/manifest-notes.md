# Manifest Notes

## Role

Remote controller only. Plugin sends IPC commands to the native app; contains no recording logic.

## Transport

localhost WebSocket/TCP JSON-RPC (JSON-RPC over TCP to 127.0.0.1:45991).
Named-pipe transport is a deferred alternative (ADR-0007).

## Plugin characteristics

- Technology: Node.js + TypeScript
- SDK: official Elgato Stream Deck SDK

## Planned actions

- `save_replay`
- `recording_start`
- `recording_stop`
- `pause_resume`

## TODO

- Finalize action UUIDs
- Define IPC request/response schema (JSON-RPC method names, params, result shapes)
- Create plugin scaffold only after native IPC contract is stable
