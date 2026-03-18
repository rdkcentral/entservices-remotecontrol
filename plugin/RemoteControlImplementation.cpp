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

#include "RemoteControlImplementation.h"
#include "libIBusDaemon.h"
#include "UtilsIarm.h"
#include "UtilsJsonRpc.h"

#include <algorithm>

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 1

#define IARM_FACTORY_RESET_TIMEOUT  (15 * 1000)  // 15 seconds, in milliseconds
#define IARM_IRDB_CALLS_TIMEOUT     (10 * 1000)  // 10 seconds, in milliseconds

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(RemoteControlImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH)

    RemoteControlImplementation* RemoteControlImplementation::_instance = nullptr;

    RemoteControlImplementation::RemoteControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
    {
        _instance = this;
    }

    RemoteControlImplementation::~RemoteControlImplementation()
    {
        _instance = nullptr;
        // Release any still-registered notification observers.
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            if (notification != nullptr) {
                 notification->Release();
            }
        }
        _notifications.clear();
        _adminLock.Unlock();

        DeinitializeIARM();

        if (_service != nullptr) {
            _service->Release();
            _service = nullptr;
        }
    }

    Core::hresult RemoteControlImplementation::Configure(PluginHost::IShell* service)
    {
        LOGINFO("Configuring RemoteControlImplementation");
        ASSERT(service != nullptr);
        ASSERT(_service == nullptr);
        _service = service;
        _service->AddRef();

        InitializeIARM();
        if (Utils::IARM::isConnected() == false) {
            LOGERR("Failed to initialize IARM for RemoteControlImplementation, configuration will fail");
            return Core::ERROR_GENERAL;
         }
        return Core::ERROR_NONE;
    }

    // ─── INotification management ───

    Core::hresult RemoteControlImplementation::Register(Exchange::IRemoteControl::INotification* notification)
    {
        ASSERT(notification != nullptr);

        _adminLock.Lock();
        auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it == _notifications.end()) {
            notification->AddRef();
            _notifications.push_back(notification);
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::Unregister(const Exchange::IRemoteControl::INotification* notification)
    {
        ASSERT(notification != nullptr);

        _adminLock.Lock();
        auto it = std::find_if(_notifications.begin(), _notifications.end(), [notification](const Exchange::IRemoteControl::INotification* current) {
            return current == notification;
        });
        if (it != _notifications.end()) {
            (*it)->Release();
            _notifications.erase(it);
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    // ─── IARM lifecycle ───

    void RemoteControlImplementation::InitializeIARM()
    {
        bool alreadyConnected = Utils::IARM::isConnected();
        if (Utils::IARM::init()) {
            _hasOwnProcess = !alreadyConnected;
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RCU_STATUS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT, remoteEventHandler) );
        } else {
            _hasOwnProcess = false;
        }
    }

    void RemoteControlImplementation::DeinitializeIARM()
    {
        if (_hasOwnProcess) {
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RCU_STATUS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE, remoteEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT, remoteEventHandler) );

            IARM_CHECK( IARM_Bus_Disconnect() );
            IARM_CHECK( IARM_Bus_Term() );
            _hasOwnProcess = false;
        }
    }

    // ─── IARM event dispatching ───

    void RemoteControlImplementation::remoteEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len)
    {
        if (_instance != nullptr) {
            _instance->iarmEventHandler(owner, eventId, data, len);
        } else {
            LOGWARN("WARNING - cannot handle btremote IARM events without a RemoteControlImplementation instance!");
        }
    }

    void RemoteControlImplementation::iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len)
    {
        if (strcmp(owner, CTRLM_MAIN_IARM_BUS_NAME)) {
            LOGERR("ERROR - unexpected event: owner %s, eventId: %d, data: %p, size: %zu.", owner, (int)eventId, data, len);
            return;
        }

        if (data == nullptr || len == 0) {
            LOGERR("ERROR - event with NO DATA: eventId: %d, data: %p, size: %zu.", (int)eventId, data, len);
            return;
        }

        ctrlm_main_iarm_event_json_t* eventData = static_cast<ctrlm_main_iarm_event_json_t*>(data);

        switch (eventId) {
            case CTRLM_RCU_IARM_EVENT_RCU_STATUS:
            case CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE:
            case CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT:
                LOGWARN("Got CTRLM_RCU_IARM_EVENT event.");
                NotifyStatus(eventData);
                break;
            case CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS:
                LOGWARN("Got CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS event.");
                NotifyValidation(eventData);
                break;
            case CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS:
                LOGWARN("Got CTRLM_RCU_IARM_FIRMWARE_EVENT event.");
                NotifyFirmwareUpdateProgress(eventData);
                break;
            default:
                LOGERR("ERROR - unexpected ctrlm event: eventId: %d, data: %p, size: %zu.", (int)eventId, data, len);
                break;
        }
    }

    // ─── Event notifications to observers ───

    void RemoteControlImplementation::NotifyStatus(ctrlm_main_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::StatusEventData status;
        status.netType = params.HasLabel("netType") ? static_cast<uint32_t>(params["netType"].Number()) : 0;
        status.netTypeSupported = params.HasLabel("netTypeSupported") ? params["netTypeSupported"].Boolean() : false;
        status.pairingState = params.HasLabel("pairingState") ? static_cast<Exchange::PairingState>(static_cast<uint8_t>(params["pairingState"].Number())) : Exchange::PairingState::IDLE;
        status.irProgState = params.HasLabel("irProgState") ? static_cast<Exchange::IRProgState>(static_cast<uint8_t>(params["irProgState"].Number())) : Exchange::IRProgState::IDLE;

        std::vector<Exchange::IRemoteControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnStatus(status);
            notification->Release();
        }
    }

    void RemoteControlImplementation::NotifyValidation(ctrlm_main_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::ValidationStatusObject status;
        status.netType = params.HasLabel("netType") ? static_cast<uint32_t>(params["netType"].Number()) : 0;
        status.validationDigit1 = params.HasLabel("validationDigit1") ? static_cast<uint32_t>(params["validationDigit1"].Number()) : 0;
        status.validationDigit2 = params.HasLabel("validationDigit2") ? static_cast<uint32_t>(params["validationDigit2"].Number()) : 0;
        status.validationDigit3 = params.HasLabel("validationDigit3") ? static_cast<uint32_t>(params["validationDigit3"].Number()) : 0;

        std::vector<Exchange::IRemoteControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnValidation(status);
            notification->Release();
        }
    }

    void RemoteControlImplementation::NotifyFirmwareUpdateProgress(ctrlm_main_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::FirmwareUpdateProgressEvent progress;
        progress.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";

        if (params.HasLabel("status")) {
            JsonObject statusObj = params["status"].Object();
            progress.status.state = statusObj.HasLabel("state") ? static_cast<Exchange::FirmwareUpdateState>(static_cast<uint8_t>(statusObj["state"].Number())) : Exchange::FirmwareUpdateState::FAILED;
            progress.status.percentComplete = statusObj.HasLabel("percentComplete") ? static_cast<uint32_t>(statusObj["percentComplete"].Number()) : 0;
        }

        std::vector<Exchange::IRemoteControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnFirmwareUpdateProgress(progress);
            notification->Release();
        }
    }

    // ─── IARM call helper ───
    // Encapsulates the boilerplate: allocate ctrlm_main_iarm_call_json_t, copy JSON payload, make the IARM bus call, parse the JSON result.
    // Returns Core::ERROR_NONE on success, Core::ERROR_GENERAL for allocation failure, or Core::ERROR_RPC_CALL_FAILED if the IARM call itself fails
    Core::hresult RemoteControlImplementation::IARMBusCall(const string& method, const string& jsonParams, JsonObject& result, int timeoutMs)
    {
        size_t totalsize = sizeof(ctrlm_main_iarm_call_json_t) + jsonParams.size() + 1;
        ctrlm_main_iarm_call_json_t* call = (ctrlm_main_iarm_call_json_t*)calloc(1, totalsize);

        if (call == nullptr) {
            LOGERR("ERROR - Cannot allocate IARM structure - size: %u.", (unsigned)totalsize);
            return Core::ERROR_GENERAL;
        }

        call->api_revision = CTRLM_MAIN_IARM_BUS_API_REVISION;
        size_t len = jsonParams.copy(call->payload, jsonParams.size());
        call->payload[len] = '\0';

        IARM_Result_t res;
        if (timeoutMs > 0) {
            res = IARM_Bus_Call_with_IPCTimeout(CTRLM_MAIN_IARM_BUS_NAME, method.c_str(), (void*)call, totalsize, timeoutMs);
        } else {
            res = IARM_Bus_Call(CTRLM_MAIN_IARM_BUS_NAME, method.c_str(), (void*)call, totalsize);
        }

        if (res != IARM_RESULT_SUCCESS) {
            LOGERR("ERROR - %s Bus Call FAILED, res: %d.", method.c_str(), (int)res);
            free(call);
            return Core::ERROR_RPC_CALL_FAILED;
        }

        result.FromString(call->result);
        free(call);
        return Core::ERROR_NONE;
    }

    // ─── IRemoteControl method implementations ───
    // The underlying IARM/ctrlm interface is JSON-based: the input parameters are serialized
    // to a JSON string, sent over IARM, and the result comes back as a JSON string.
    // Each method below converts its typed struct parameters into JSON, calls IARM, and
    // then parses the JSON result back into the typed response.

    Core::hresult RemoteControlImplementation::GetApiVersionNumber(Exchange::GetApiVersionNumberResponse& response)
    {
        response.version = 1;
        response.success = true;
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::StartPairing(const Exchange::StartPairingRequest& request, bool& success, Exchange::IStringIterator* const /* macAddressList */)
    {
        JsonObject params;
        params["netType"] = request.netType;
        params["timeout"] = request.timeout;
        params["screenBindEnable"] = request.screenBindEnable;
        params["scanEnable"] = request.scanEnable;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_START_PAIRING, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::StopPairing(const Exchange::StopPairingRequest& request, bool& success)
    {
        JsonObject params;
        params["screenBindDisable"] = request.screenBindDisable;
        params["scanDisable"] = request.scanDisable;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_STOP_PAIRING, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetNetStatus(const Exchange::GetNetStatusRequest& request, Exchange::GetNetStatusResponse& response, Exchange::IUint32Iterator*& netTypeSupported, Exchange::IRemoteDataIterator*& remoteData)
    {
        JsonObject params;
        params["netType"] = request.netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_GET_RCU_STATUS, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.netType = result.HasLabel("netType") ? static_cast<uint32_t>(result["netType"].Number()) : request.netType;
        response.pairingState = result.HasLabel("pairingState") ? static_cast<Exchange::PairingState>(static_cast<uint8_t>(result["pairingState"].Number())) : Exchange::PairingState::IDLE;
        response.irProgState = result.HasLabel("irProgState") ? static_cast<Exchange::IRProgState>(static_cast<uint8_t>(result["irProgState"].Number())) : Exchange::IRProgState::IDLE;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        // Parse netTypeSupported array
        std::list<uint32_t> supportedTypes;
        if (result.HasLabel("netTypeSupported")) {
            auto arr = result["netTypeSupported"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                supportedTypes.push_back(static_cast<uint32_t>(arr[i].Number()));
            }
        }
        netTypeSupported = Core::Service<RPC::ValueIterator>::Create<Exchange::IUint32Iterator>(supportedTypes);

        // Parse remoteData array
        std::list<Exchange::RemoteData> remotes;
        if (result.HasLabel("remoteData")) {
            auto arr = result["remoteData"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                JsonObject rObj = arr[i].Object();
                Exchange::RemoteData rd;
                rd.macAddress = rObj.HasLabel("macAddress") ? rObj["macAddress"].String() : "";
                rd.connected = rObj.HasLabel("connected") ? rObj["connected"].Boolean() : false;
                rd.name = rObj.HasLabel("name") ? rObj["name"].String() : "";
                rd.remoteId = rObj.HasLabel("remoteId") ? static_cast<uint32_t>(rObj["remoteId"].Number()) : 0;
                rd.deviceId = rObj.HasLabel("deviceId") ? static_cast<uint32_t>(rObj["deviceId"].Number()) : 0;
                rd.make = rObj.HasLabel("make") ? rObj["make"].String() : "";
                rd.model = rObj.HasLabel("model") ? rObj["model"].String() : "";
                rd.hwVersion = rObj.HasLabel("hwVersion") ? rObj["hwVersion"].String() : "";
                rd.swVersion = rObj.HasLabel("swVersion") ? rObj["swVersion"].String() : "";
                rd.btlVersion = rObj.HasLabel("btlVersion") ? rObj["btlVersion"].String() : "";
                rd.serialNumber = rObj.HasLabel("serialNumber") ? rObj["serialNumber"].String() : "";
                rd.batteryPercent = rObj.HasLabel("batteryPercent") ? static_cast<uint8_t>(rObj["batteryPercent"].Number()) : 0;
                rd.tvIRCode = rObj.HasLabel("tvIRCode") ? rObj["tvIRCode"].String() : "";
                rd.ampIRCode = rObj.HasLabel("ampIRCode") ? rObj["ampIRCode"].String() : "";
                rd.wakeupKeyCode = rObj.HasLabel("wakeupKeyCode") ? static_cast<uint32_t>(rObj["wakeupKeyCode"].Number()) : 0;
                rd.wakeupConfig = rObj.HasLabel("wakeupConfig") ? static_cast<Exchange::WakeupConfig>(static_cast<uint8_t>(rObj["wakeupConfig"].Number())) : Exchange::WakeupConfig::ALL;
                rd.wakeupCustomList = rObj.HasLabel("wakeupCustomList") ? rObj["wakeupCustomList"].String() : "";
                rd.upgradeSessionId = rObj.HasLabel("upgradeSessionId") ? rObj["upgradeSessionId"].String() : "";
                remotes.push_back(rd);
            }
        }
        remoteData = Core::Service<RPC::IteratorType<Exchange::IRemoteDataIterator>>::Create<Exchange::IRemoteDataIterator>(remotes);

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetIRDBManufacturers(const Exchange::GetIRDBManufacturersRequest& request, Exchange::GetIRDBManufacturersResponse& response, Exchange::IStringIterator*& manufacturers)
    {
        JsonObject params;
        params["avDevType"] = static_cast<uint8_t>(request.avDevType);
        params["manufacturer"] = request.manufacturer;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_MANUFACTURERS, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.avDevType = request.avDevType;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> mfrs;
        if (result.HasLabel("manufacturers")) {
            auto arr = result["manufacturers"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                mfrs.push_back(arr[i].String());
            }
        }
        manufacturers = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(mfrs);

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetIRDBModels(const Exchange::GetIRDBModelsRequest& request, Exchange::GetIRDBModelsResponse& response, Exchange::IStringIterator*& models)
    {
        JsonObject params;
        params["avDevType"] = static_cast<uint8_t>(request.avDevType);
        params["manufacturer"] = request.manufacturer;
        params["model"] = request.model;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_MODELS, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.avDevType = request.avDevType;
        response.manufacturer = request.manufacturer;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> mdls;
        if (result.HasLabel("models")) {
            auto arr = result["models"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                mdls.push_back(arr[i].String());
            }
        }
        models = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(mdls);

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetIRCodesByAutoLookup(const Exchange::GetNetStatusRequest& request, Exchange::GetIRCodesByAutoLookupResponse& response, Exchange::IStringIterator*& tvCodes, Exchange::IStringIterator*& avrCodes)
    {
        JsonObject params;
        params["netType"] = request.netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_AUTO_LOOKUP, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.tvManufacturer = result.HasLabel("tvManufacturer") ? result["tvManufacturer"].String() : "";
        response.tvModel = result.HasLabel("tvModel") ? result["tvModel"].String() : "";
        response.avrManufacturer = result.HasLabel("avrManufacturer") ? result["avrManufacturer"].String() : "";
        response.avrModel = result.HasLabel("avrModel") ? result["avrModel"].String() : "";
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> tv;
        if (result.HasLabel("tvCodes")) {
            auto arr = result["tvCodes"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                tv.push_back(arr[i].String());
            }
        }
        tvCodes = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(tv);

        std::list<string> avr;
        if (result.HasLabel("avrCodes")) {
            auto arr = result["avrCodes"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                avr.push_back(arr[i].String());
            }
        }
        avrCodes = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(avr);

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetIRCodesByNames(const Exchange::GetIRDBModelsRequest& request, Exchange::GetIRCodesByNamesResponse& response, Exchange::IStringIterator*& codes)
    {
        JsonObject params;
        params["avDevType"] = static_cast<uint8_t>(request.avDevType);
        params["manufacturer"] = request.manufacturer;
        params["model"] = request.model;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_CODES, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.avDevType = request.avDevType;
        response.manufacturer = request.manufacturer;
        response.model = request.model;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> codeList;
        if (result.HasLabel("codes")) {
            auto arr = result["codes"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                codeList.push_back(arr[i].String());
            }
        }
        codes = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(codeList);

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::SetIRCode(const Exchange::SetIRCodeRequest& request, bool& success)
    {
        JsonObject params;
        params["remoteId"] = request.remoteId;
        params["netType"] = request.netType;
        params["avDevType"] = static_cast<uint8_t>(request.avDevType);
        params["code"] = request.code;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_SET_CODE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::ClearIRCodes(const Exchange::ClearIRCodesRequest& request, bool& success)
    {
        JsonObject params;
        params["remoteId"] = request.remoteId;
        params["netType"] = request.netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_CLEAR_CODE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::GetLastKeypressSource(Exchange::GetLastKeypressSourceResponse& response)
    {
        JsonObject params;
        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_LAST_KEYPRESS_GET, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.controllerId = result.HasLabel("controllerId") ? static_cast<uint32_t>(result["controllerId"].Number()) : 0;
        response.timestamp = result.HasLabel("timestamp") ? static_cast<uint64_t>(result["timestamp"].Number()) : 0;
        response.sourceName = result.HasLabel("sourceName") ? result["sourceName"].String() : "";
        response.sourceType = result.HasLabel("sourceType") ? result["sourceType"].String() : "";
        response.sourceKeyCode = result.HasLabel("sourceKeyCode") ? static_cast<uint32_t>(result["sourceKeyCode"].Number()) : 0;
        response.isScreenBindMode = result.HasLabel("isScreenBindMode") ? result["isScreenBindMode"].Boolean() : false;
        response.remoteKeypadConfig = result.HasLabel("remoteKeypadConfig") ? static_cast<uint32_t>(result["remoteKeypadConfig"].Number()) : 0;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::ConfigureWakeupKeys(const Exchange::ConfigureWakeupKeysRequest& request, bool& success)
    {
        JsonObject params;
        params["wakeupConfig"] = static_cast<uint8_t>(request.wakeupConfig);
        if (!request.customKeys.empty()) {
            params["customKeys"] = request.customKeys;
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_WRITE_RCU_WAKEUP_CONFIG, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::InitializeIRDB(const Exchange::GetNetStatusRequest& request, bool& success)
    {
        JsonObject params;
        params["netType"] = request.netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_INITIALIZE, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::FindMyRemote(const Exchange::FindMyRemoteRequest& request, bool& success)
    {
        JsonObject params;
        params["level"] = static_cast<uint8_t>(request.level);

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_FIND_MY_REMOTE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::FactoryReset(bool& success)
    {
        JsonObject params;
        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_FACTORY_RESET, jsonParams, result, IARM_FACTORY_RESET_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::Unpair(bool& success, Exchange::IStringIterator* const /* macAddressList */)
    {
        JsonObject params;
        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_UNPAIR, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::StartFirmwareUpdate(const Exchange::StartFirmwareUpdateRequest& request, bool& success, Exchange::IStringIterator*& sessionIdList)
    {
        JsonObject params;
        params["macAddress"] = request.macAddress;
        params["fileName"] = request.fileName;
        params["fileType"] = request.fileType;
        params["percentIncrement"] = request.percentIncrement;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_START_FIRMWARE_UPDATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> sessions;
        if (result.HasLabel("sessionIdList")) {
            auto arr = result["sessionIdList"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                sessions.push_back(arr[i].String());
            }
        }
        sessionIdList = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(sessions);

        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::CancelFirmwareUpdate(const Exchange::CancelFirmwareUpdateRequest& request, bool& success)
    {
        JsonObject params;
        params["sessionId"] = request.sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_CANCEL_FIRMWARE_UPDATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result["success"].Boolean();
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult RemoteControlImplementation::StatusFirmwareUpdate(const Exchange::CancelFirmwareUpdateRequest& request, Exchange::StatusFirmwareUpdateResponse& response)
    {
        JsonObject params;
        params["sessionId"] = request.sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_STATUS_FIRMWARE_UPDATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            return callResult;
        }

        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        if (result.HasLabel("result")) {
            JsonObject statusObj = result["result"].Object();
            response.result.state = statusObj.HasLabel("state") ? static_cast<Exchange::FirmwareUpdateState>(static_cast<uint8_t>(statusObj["state"].Number())) : Exchange::FirmwareUpdateState::FAILED;
            response.result.percentComplete = statusObj.HasLabel("percentComplete") ? static_cast<uint32_t>(statusObj["percentComplete"].Number()) : 0;
        }

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

} // namespace Plugin
} // namespace WPEFramework
