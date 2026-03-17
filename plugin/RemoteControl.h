#pragma once

#include "Module.h"
#include <interfaces/IRemoteControl.h>
#include <interfaces/json/JRemoteControl.h>
#include <interfaces/IConfiguration.h>

namespace WPEFramework {
namespace Plugin {

    class RemoteControl : public PluginHost::IPlugin, public PluginHost::JSONRPC {
    public:
        RemoteControl(const RemoteControl&) = delete;
        RemoteControl& operator=(const RemoteControl&) = delete;

        RemoteControl()
            : _implementation(nullptr)
            , _connectionId(0)
            , _service(nullptr)
            , _connectionNotification(this)
            , _notification(this)
        {
        }
        ~RemoteControl() override = default;

        BEGIN_INTERFACE_MAP(RemoteControl)
            INTERFACE_ENTRY(PluginHost::IPlugin)
            INTERFACE_ENTRY(PluginHost::IDispatcher)
            INTERFACE_AGGREGATE(Exchange::IRemoteControl, _implementation)
        END_INTERFACE_MAP

        // IPlugin methods
        const string Initialize(PluginHost::IShell* service) override;
        void Deinitialize(PluginHost::IShell* service) override;
        string Information() const override { return {}; }

    private:
        class ConnectionNotification : public RPC::IRemoteConnection::INotification {
        public:
            explicit ConnectionNotification(RemoteControl* parent) : _parent(*parent) {}
            ~ConnectionNotification() override = default;

            void Activated(RPC::IRemoteConnection*) override {}
            void Deactivated(RPC::IRemoteConnection* connection) override {
                _parent.Deactivated(connection);
            }

            BEGIN_INTERFACE_MAP(ConnectionNotification)
                INTERFACE_ENTRY(RPC::IRemoteConnection::INotification)
            END_INTERFACE_MAP

        private:
            RemoteControl& _parent;
        };

        class Notification : public Exchange::IRemoteControl::INotification {
        public:
            explicit Notification(RemoteControl* parent) : _parent(*parent) {}
            ~Notification() override = default;

            void OnStatus(const Exchange::StatusEventData& status) override {
                Exchange::JRemoteControl::Event::OnStatus(_parent, status);
            }
            void OnValidation(const Exchange::ValidationStatusObject& status) override {
                Exchange::JRemoteControl::Event::OnValidation(_parent, status);
            }
            void OnFirmwareUpdateProgress(const Exchange::FirmwareUpdateProgressEvent& progress) override {
                Exchange::JRemoteControl::Event::OnFirmwareUpdateProgress(_parent, progress);
            }

            BEGIN_INTERFACE_MAP(Notification)
                INTERFACE_ENTRY(Exchange::IRemoteControl::INotification)
            END_INTERFACE_MAP

        private:
            RemoteControl& _parent;
        };

        void Deactivated(RPC::IRemoteConnection* connection);

        Exchange::IRemoteControl* _implementation;
        uint32_t _connectionId;
        PluginHost::IShell* _service;
        Core::Sink<ConnectionNotification> _connectionNotification;
        Core::Sink<Notification> _notification;
        Exchange::IConfiguration* _configure{};
    };

} // namespace Plugin
} // namespace WPEFramework
