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

```bash
mkdir build && cd build
cmake ..
make
```

## Dependencies

- WPE Framework
- IARM Bus
- CTRLM
- jsoncpp

## Testing

A test client is available in the `plugin/test` directory for validating plugin functionality.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
