# RemoteControl Plugin

A WPE Framework plugin that provides remote control management for RDK devices. Enables pairing, configuration, and management of IR, RF, and BLE remote controls through a unified JSON-RPC interface.

## Overview

The RemoteControl service manages all aspects of remote control functionality including:
- Remote control pairing and unpairing
- IR database management for TV control
- Network status and diagnostics
- Power management (wake-up key configuration)
- Remote locator functionality (Find My Remote)

## Key Features

- **Multi-Remote Support**: Manage IR, RF, and BLE remote controls
- **IR Database**: Comprehensive database of TV manufacturer codes
- **Pairing Management**: Simple start/stop pairing workflow
- **Battery Monitoring**: Track remote control battery status
- **Network Status**: Query remote connectivity and signal strength
- **Factory Reset**: Remote configuration reset capability

## Architecture

The plugin integrates with the CTRLM (Control Manager) subsystem via IARM bus, providing a JSON-RPC interface for client applications to manage remote controls.

For detailed architecture information, see [ARCHITECTURE.md](ARCHITECTURE.md).

For product features and capabilities, see [PRODUCT.md](PRODUCT.md).

## Building

### Yocto/BitBake Build

```bash
bitbake thunder-plugins
```

### Manual Build

```bash
mkdir build && cd build
cmake ..
make
```

## Testing

### API Version and Quirks

Test common plugin methods:

```bash
# Get API version number
curl -d '{"jsonrpc":"2.0","id":"4","method":"org.rdk.RemoteControl.1.getApiVersionNumber"}' http://127.0.0.1:9998/jsonrpc

# Get plugin quirks
curl --header "Content-Type: application/json" --request POST --data '{"jsonrpc":"2.0","id":"3","method": "org.rdk.RemoteControl.1.getQuirks"}' http://127.0.0.1:9998/jsonrpc
```

### Remote Control Examples

```bash
# Get network status
curl -d '{"jsonrpc":"2.0","id":"5","method":"org.rdk.RemoteControl.1.getNetStatus"}' http://127.0.0.1:9998/jsonrpc

# Get IR database manufacturers
# Get IR database manufacturers (netType, avDevType, and manufacturer are required)
curl -d '{"jsonrpc":"2.0","id":"6","method":"org.rdk.RemoteControl.1.getIRDBManufacturers","params":{"netType":1,"avDevType":"TV","manufacturer":"Sam"}}' http://127.0.0.1:9998/jsonrpc
```

### Test Client

A test client is available in the `plugin/test` directory for validating plugin functionality.

## Dependencies

- WPE Framework
- IARM Bus
- CTRLM
- jsoncpp

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
