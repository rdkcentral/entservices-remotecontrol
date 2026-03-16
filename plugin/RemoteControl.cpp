#include "RemoteControl.h"

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<RemoteControl> metadata(1, 0, 0, {}, {}, {});
    }

    const string RemoteControl::Initialize(PluginHost::IShell* service)
    {
        string message;
        _service = service;
        _service->AddRef();
        _service->Register(&_connectionNotification);

        // Instantiate the out-of-process implementation (e.g. RemoteControlImplementation)
        _implementation = _service->Root<Exchange::IRemoteControl>(_connectionId, 2000, _T("RemoteControlImplementation"));
        if (_implementation == nullptr) {
            message = _T("RemoteControl could not be instantiated");
        } else {
            _implementation->Register(&_notification);
            Exchange::JRemoteControl::Register(*this, _implementation);
        }
        return message;
    }

    void RemoteControl::Deinitialize(PluginHost::IShell* service)
    {
        if (_implementation != nullptr) {
            Exchange::JRemoteControl::Unregister(*this);
            _implementation->Unregister(&_notification);

            RPC::IRemoteConnection* connection = _service->RemoteConnection(_connectionId);
            _implementation->Release();
            _implementation = nullptr;
            if (connection != nullptr) {
                connection->Terminate();
                connection->Release();
            }
        }
        _service->Unregister(&_connectionNotification);
        _service->Release();
        _service = nullptr;
        _connectionId = 0;
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
