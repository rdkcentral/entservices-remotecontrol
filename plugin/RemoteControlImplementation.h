/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2023 RDK Management
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
        Core::hresult StartPairing(const Exchange::StartPairingRequest& request, bool& success, Exchange::IStringIterator* const macAddressList) override;
        Core::hresult StopPairing(const Exchange::StopPairingRequest& request, bool& success) override;
        Core::hresult GetNetStatus(const Exchange::GetNetStatusRequest& request, Exchange::GetNetStatusResponse& response, Exchange::IUint32Iterator*& netTypeSupported, Exchange::IRemoteDataIterator*& remoteData) override;
        Core::hresult GetIRDBManufacturers(const Exchange::GetIRDBManufacturersRequest& request, Exchange::GetIRDBManufacturersResponse& response, Exchange::IStringIterator*& manufacturers) override;
        Core::hresult GetIRDBModels(const Exchange::GetIRDBModelsRequest& request, Exchange::GetIRDBModelsResponse& response, Exchange::IStringIterator*& models) override;
        Core::hresult GetIRCodesByAutoLookup(const Exchange::GetIRCodesByAutoLookupRequest& request, Exchange::GetIRCodesByAutoLookupResponse& response, Exchange::IStringIterator*& tvCodes, Exchange::IStringIterator*& avrCodes) override;
        Core::hresult GetIRCodesByNames(const Exchange::GetIRCodesByNamesRequest& request, Exchange::GetIRCodesByNamesResponse& response, Exchange::IStringIterator*& codes) override;
        Core::hresult SetIRCode(const Exchange::SetIRCodeRequest& request, bool& success) override;
        Core::hresult ClearIRCodes(const Exchange::ClearIRCodesRequest& request, bool& success) override;
        Core::hresult GetLastKeypressSource(Exchange::GetLastKeypressSourceResponse& response) override;
        Core::hresult ConfigureWakeupKeys(const Exchange::ConfigureWakeupKeysRequest& request, bool& success) override;
        Core::hresult InitializeIRDB(const Exchange::InitializeIRDBRequest& request, bool& success) override;
        Core::hresult FindMyRemote(const Exchange::FindMyRemoteRequest& request, bool& success) override;
        Core::hresult FactoryReset(bool& success) override;
        Core::hresult Unpair(bool& success, Exchange::IStringIterator* const macAddressList) override;
        Core::hresult StartFirmwareUpdate(const Exchange::StartFirmwareUpdateRequest& request, bool& success, Exchange::IStringIterator*& sessionIdList) override;
        Core::hresult CancelFirmwareUpdate(const Exchange::CancelFirmwareUpdateRequest& request, bool& success) override;
        Core::hresult StatusFirmwareUpdate(const Exchange::StatusFirmwareUpdateRequest& request, Exchange::StatusFirmwareUpdateResponse& response) override;

        virtual Core::hresult Register(Exchange::IRemoteControl::INotification* notification) override;
        virtual Core::hresult Unregister(const Exchange::IRemoteControl::INotification* notification) override;

        // IConfiguration interface
        uint32_t Configure(PluginHost::IShell* service) override;

    private:
        void InitializeIARM();
        void DeinitializeIARM();

        static void remoteEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);
        void iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);

        void NotifyStatus(ctrlm_main_iarm_event_json_t* eventData);
        void NotifyValidation(ctrlm_main_iarm_event_json_t* eventData);
        void NotifyFirmwareUpdateProgress(ctrlm_main_iarm_event_json_t* eventData);

        Core::hresult IARMBusCall(const string& method, const string& jsonParams, JsonObject& result, int timeoutMs = 0);

        Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        std::vector<Exchange::IRemoteControl::INotification*> _notifications;
        bool _hasOwnProcess;

        static RemoteControlImplementation* _instance;
    };

} // namespace Plugin
} // namespace WPEFramework
