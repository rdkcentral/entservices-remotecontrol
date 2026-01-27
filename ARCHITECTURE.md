# RemoteControl Plugin Architecture

## Overview

The RemoteControl plugin is a WPE Framework service that provides remote control management functionality for RDK devices. It acts as an interface between client applications and the CTRLM (Control Manager) subsystem through IARM (Inter Application Resource Manager) bus communication.

## System Architecture

### High-Level Components

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Client Apps   │◄──►│ RemoteControl    │◄──►│ CTRLM Subsystem │
│   (JSON-RPC)    │    │   WPE Plugin     │    │  (via IARM)     │
└─────────────────┘    └──────────────────┘    └─────────────────┘
                              │
                              ▼
                       ┌─────────────┐
                       │ IARM Bus    │
                       └─────────────┘
```

### Core Components

#### 1. JSON-RPC Interface
- **Purpose**: Provides standardized API endpoints for remote control operations
- **Location**: `plugin/RemoteControl.cpp`
- **Key Methods**:
    - `startPairing()` / `stopPairing()` - Remote control pairing management
    - `getNetStatus()` - Network status for remote controls
    - `getIRDBManufacturers()` / `getIRDBModels()` - IR database queries
    - `setIRCode()` / `clearIRCodes()` - IR code management
    - `configureWakeupKeys()` - Power management integration
    - `findMyRemote()` - Remote locator functionality
    - `factoryReset()` - Device reset operations

#### 2. IARM Communication Layer
- **Purpose**: Interfaces with system-level control manager (CTRLM)
- **Protocol**: Uses IARM bus for inter-process communication
- **Key Features**:
    - Asynchronous event handling
    - Timeout management for critical operations
    - Error propagation and handling

#### 3. CTRLM Integration
- **Purpose**: Provides low-level remote control hardware management
- **Dependencies**:
    - `ctrlm_ipc.h` - Main control manager interface
    - `ctrlm_ipc_rcu.h` - Remote Control Unit specific operations
    - `ctrlm_ipc_ble.h` - Bluetooth Low Energy support
- **Functionality**:
    - Remote pairing and unpairing
    - IR database management
    - Battery status monitoring
    - RF/BLE communication handling

## Software Architecture

### Plugin Framework Integration
The RemoteControl plugin inherits from WPE Framework's `PluginHost::JSONRPC` class, providing:
- Automatic JSON-RPC method registration and dispatch
- Standardized error handling and response formatting
- Plugin lifecycle management (Initialize/Deinitialize)
- Configuration management through WPE Framework

### Threading Model
- **Main Thread**: JSON-RPC request handling
- **IARM Thread**: Asynchronous system communication
- **Event Thread**: Remote control event processing

### Error Handling
- Comprehensive error codes for different failure scenarios
- Timeout mechanisms for system calls
- Graceful degradation when hardware is unavailable
- Detailed logging for debugging and monitoring

## Configuration

### Build-time Configuration
- CMake options in `plugin/CMakeLists.txt`
- Conditional compilation based on CTRLM availability
- Optional features through preprocessor definitions

### Runtime Configuration
- Plugin configuration via `RemoteControl.config`
- IARM bus configuration
- CTRLM subsystem parameters

## Dependencies

### System Dependencies
- **IARM Bus**: Inter-application communication framework
- **CTRLM**: Control Manager for remote control hardware
- **WPE Framework**: Plugin hosting framework

### Utility Dependencies
- `UtilsJsonRpc.h` - JSON-RPC helper functions
- `UtilsIarm.h` - IARM communication utilities
- `UtilsString.h` - String manipulation utilities
- `UtilsLogging.h` - Centralized logging framework

## Data Flow

1. **Client Request**: JSON-RPC request received from client application
2. **Parameter Validation**: Request parameters validated and parsed
3. **IARM Communication**: Request translated to IARM call to CTRLM
4. **Hardware Interaction**: CTRLM interfaces with remote control hardware
5. **Response Processing**: Hardware response processed and formatted
6. **Client Response**: JSON-RPC response sent back to client

## Security Considerations

- Input validation on all JSON-RPC parameters
- Timeout protection against hanging system calls
- Error information sanitization
- Access control through WPE Framework security model

## Performance Characteristics

- Low-latency for common operations (status queries)
- Configurable timeouts for long-running operations (pairing, factory reset)
- Efficient event handling to prevent blocking
- Memory-optimized data structures for frequent operations
