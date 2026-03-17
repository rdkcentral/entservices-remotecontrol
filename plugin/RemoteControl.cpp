#include "RemoteControl.h"

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<RemoteControl> metadata(1, 0, 0, {}, {}, {});
    }

    const string RemoteControl::Initialize(PluginHost::IShell* service)
    {
        string message;

        ASSERT(service != nullptr);
        ASSERT(_service == nullptr);
        ASSERT(_implementation == nullptr);
        ASSERT(_connectionId == 0);

        _service = service;
        _service->AddRef();
        _service->Register(&_connectionNotification);

        // Instantiate the out-of-process implementation (e.g. RemoteControlImplementation)
        _implementation = _service->Root<Exchange::IRemoteControl>(_connectionId, 2000, _T("RemoteControlImplementation"));

        if (nullptr != _implementation)
        {
            _configure = _implementation->QueryInterface<Exchange::IConfiguration>();
            if (_configure != nullptr)
            {
                uint32_t result = _configure->Configure(_service);
                if (result != Core::ERROR_NONE)
                {
                    message = _T("RemoteControl could not be configured");
                }
            }
            else
            {
                message = _T("RemoteControl implementation did not provide a configuration interface");
            }

            _implementation->Register(&_notification);
            Exchange::JRemoteControl::Register(*this, _implementation);
        }
        else
        {
            message = _T("RemoteControl could not be instantiated");
        }

        return message;
    }

    void RemoteControl::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        _service->Unregister(&_connectionNotification);

        if (nullptr != _implementation)
        {
            _implementation->Unregister(&_notification);
            Exchange::JRemoteControl::Unregister(*this);

            if (_configure != nullptr) {
                _configure->Release();
                _configure = nullptr;
            }

            RPC::IRemoteConnection* connection = service->RemoteConnection(_connectionId);
            VARIABLE_IS_NOT_USED uint32_t result = _implementation->Release();
            _implementation = nullptr;

            ASSERT(result == Core::ERROR_DESTRUCTION_SUCCEEDED);

            if (nullptr != connection)
            {
                connection->Terminate();
                connection->Release();
            }
        }

        _connectionId = 0;
        _service->Release();
        _service = nullptr;
    }

    void RemoteControl::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (connection->Id() == _connectionId) {
            Core::IWorkerPool::Instance().Submit(
                PluginHost::IShell::Job::Create(_service,
                    PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
        }
    }

} // namespace Plugin
} // namespace WPEFramework
