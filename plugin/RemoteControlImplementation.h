/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2026 RDK Management
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#pragma once

#include "Module.h"
#include <interfaces/IRemoteControl.h>
#include <interfaces/IConfiguration.h>
#include <vector>
#include "libIBus.h"
#include "ctrlm_ipc.h"
#include "ctrlm_ipc_rcu.h"
#include "ctrlm_ipc_ble.h"

namespace WPEFramework {
namespace Plugin {

    class RemoteControlImplementation : public Exchange::IRemoteControl, public Exchange::IConfiguration {
    public:
        RemoteControlImplementation(const RemoteControlImplementation&) = delete;
        RemoteControlImplementation& operator=(const RemoteControlImplementation&) = delete;

        RemoteControlImplementation();
        ~RemoteControlImplementation() override;

        BEGIN_INTERFACE_MAP(RemoteControlImplementation)
            INTERFACE_ENTRY(Exchange::IRemoteControl)
            INTERFACE_ENTRY(Exchange::IConfiguration)
        END_INTERFACE_MAP

        // IRemoteControl methods
        Core::hresult GetApiVersionNumber(Exchange::GetApiVersionNumberResponse& response) override;
        Core::hresult StartPairing(const uint32_t netType, const uint32_t timeout, const bool screenBindEnable, const bool scanEnable, bool& success, Exchange::IStringIterator* const macAddressList) override;
        Core::hresult StopPairing(const bool screenBindDisable, const bool scanDisable, bool& success) override;
        Core::hresult GetNetStatus(const uint32_t netType, Exchange::GetNetStatusResponse& response, Exchange::IUint32Iterator*& netTypeSupported, Exchange::IRemoteDataIterator*& remoteData) override;
        Core::hresult GetIRDBManufacturers(const Exchange::AVDevType avDevType, const string& manufacturer, Exchange::GetIRDBManufacturersResponse& response /* @out */, Exchange::IStringIterator*& manufacturers) override;
        Core::hresult GetIRDBModels(const Exchange::AVDevType avDevType, const string& manufacturer, const string& model, Exchange::GetIRDBModelsResponse& response, Exchange::IStringIterator*& models) override;
        Core::hresult GetIRCodesByAutoLookup(const uint32_t netType, Exchange::GetIRCodesByAutoLookupResponse& response, Exchange::IStringIterator*& tvCodes, Exchange::IStringIterator*& avrCodes) override;
        Core::hresult GetIRCodesByNames(const Exchange::AVDevType avDevType, const string& manufacturer, const string& model, Exchange::GetIRCodesByNamesResponse& response, Exchange::IStringIterator*& codes) override;
        Core::hresult SetIRCode(const uint32_t remoteId, const uint32_t netType, const Exchange::AVDevType avDevType, const string& code, bool& success) override;
        Core::hresult ClearIRCodes(const uint32_t remoteId, const uint32_t netType, bool& success) override;
        Core::hresult GetLastKeypressSource(Exchange::GetLastKeypressSourceResponse& response) override;
        Core::hresult ConfigureWakeupKeys(const Exchange::WakeupConfig wakeupConfig, const string& customKeys, bool& success) override;
        Core::hresult InitializeIRDB(const uint32_t netType, bool& success) override;
        Core::hresult FindMyRemote(const Exchange::FindMyRemoteLevel level, bool& success) override;
        Core::hresult FactoryReset(bool& success) override;
        Core::hresult Unpair(bool& success, Exchange::IStringIterator* const macAddressList) override;
        Core::hresult StartFirmwareUpdate(const string& macAddress, const string& fileName, const string& fileType, const uint32_t percentIncrement, bool& success, Exchange::IStringIterator*& sessionIdList) override;
        Core::hresult CancelFirmwareUpdate(const string& sessionId, bool& success) override;
        Core::hresult StatusFirmwareUpdate(const string& sessionId, Exchange::StatusFirmwareUpdateResponse& response) override;

        virtual Core::hresult Register(Exchange::IRemoteControl::INotification* notification) override;
        virtual Core::hresult Unregister(const Exchange::IRemoteControl::INotification* notification) override;

        // IConfiguration interface
        Core::hresult Configure(PluginHost::IShell* service) override;

    private:
        bool InitializeIARM();
        void DeinitializeIARM();

        static void remoteEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);
        void iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);

        void NotifyStatus(ctrlm_main_iarm_event_json_t* eventData);
        void NotifyValidation(ctrlm_main_iarm_event_json_t* eventData);
        void NotifyFirmwareUpdateProgress(ctrlm_main_iarm_event_json_t* eventData);

        std::vector<Exchange::IRemoteControl::INotification*> ObserverSnapshot();
        void ReleaseObserverSnapshot(std::vector<Exchange::IRemoteControl::INotification*>& observers);

        Core::hresult IARMBusCall(const string& method, const string& jsonParams, JsonObject& result, int timeoutMs = 0);

        Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        std::vector<Exchange::IRemoteControl::INotification*> _notifications;
        bool _hasOwnProcess;
        uint8_t _handlersRegistered;

        static RemoteControlImplementation* _instance;
    };

} // namespace Plugin
} // namespace WPEFramework
