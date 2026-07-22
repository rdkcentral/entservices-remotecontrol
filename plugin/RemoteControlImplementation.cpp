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
#include "PluginVersion.h"
#include "libIBusDaemon.h"
#include "UtilsIarm.h"

#include <algorithm>
#include <list>

#define IARM_FACTORY_RESET_TIMEOUT  (15 * 1000)  // 15 seconds, in milliseconds
#define IARM_IRDB_CALLS_TIMEOUT     (10 * 1000)  // 10 seconds, in milliseconds

namespace WPEFramework {
namespace Plugin {

    namespace {
        // ─── Enum ↔ string helpers for ctrlm IARM serialization ───
        // ctrlm expects/sends string representations of enums over JSON.
        // These templated helpers centralize the conversions.

        template <typename E>
        const char* enumToString(E value);

        template <typename E>
        E stringToEnum(const string& str, E defaultValue);

        string jsonValueToString(const JsonValue& value)
        {
            return value.String();
        }

        // Logs an error if 'value' exceeds the COM-RPC @restrict limit for 
        // 'fieldName'. The limit must match the @restrict annotation on the
        // corresponding parameter in IRemoteControl.h so that the generated proxy
        // stub's SetText<T> cast never silently discards bytes.
        void checkRestrictLimit(string& value, size_t limit, const char* fieldName)
        {
            if (value.size() > limit) {
                LOGERR("COM-RPC field '%s' exceeds @restrict limit: %zu > %zu bytes — truncating", fieldName, value.size(), limit);
                value.resize(limit);
            }
        }

        // --- WakeupConfig: ctrlm expects lowercase "all"/"none"/"custom" ---
        template <>
        const char* enumToString<Exchange::WakeupConfig>(Exchange::WakeupConfig value) {
            switch (value) {
                case Exchange::WakeupConfig::INVALID: return "";
                case Exchange::WakeupConfig::ALL:    return "all";
                case Exchange::WakeupConfig::NONE:   return "none";
                case Exchange::WakeupConfig::CUSTOM: return "custom";
                default:                             return "";
            }
        }

        // --- FindMyRemoteLevel: ctrlm expects lowercase "off"/"mid"/"high" ---
        template <>
        const char* enumToString<Exchange::FindMyRemoteLevel>(Exchange::FindMyRemoteLevel value) {
            switch (value) {
                case Exchange::FindMyRemoteLevel::INVALID: return "";
                case Exchange::FindMyRemoteLevel::OFF:  return "off";
                case Exchange::FindMyRemoteLevel::MID:  return "mid";
                case Exchange::FindMyRemoteLevel::HIGH: return "high";
                default:                                return "";
            }
        }

        // --- AVDevType: ctrlm expects uppercase "TV"/"AMP" ---
        template <>
        const char* enumToString<Exchange::AVDevType>(Exchange::AVDevType value) {
            switch (value) {
                case Exchange::AVDevType::INVALID: return "";
                case Exchange::AVDevType::TV:  return "TV";
                case Exchange::AVDevType::AMP: return "AMP";
                default:                       return "";
            }
        }

        template <>
        Exchange::AVDevType stringToEnum<Exchange::AVDevType>(const string& str, Exchange::AVDevType defaultValue) {
            if (str == "TV") {
                return Exchange::AVDevType::TV;
            }
            if (str == "AMP") {
                return Exchange::AVDevType::AMP;
            }
            if (str == "INVALID" || str.empty()) {
                return Exchange::AVDevType::INVALID;
            }
            return defaultValue;
        }

        bool isValidRequestEnum(const Exchange::AVDevType value)
        {
            return value != Exchange::AVDevType::INVALID;
        }

        bool isValidRequestEnum(const Exchange::WakeupConfig value)
        {
            return value != Exchange::WakeupConfig::INVALID;
        }

        bool isValidRequestEnum(const Exchange::FindMyRemoteLevel value)
        {
            return value != Exchange::FindMyRemoteLevel::INVALID;
        }

        // --- PairingState: ctrlm sends uppercase strings ---
        template <>
        Exchange::PairingState stringToEnum<Exchange::PairingState>(const string& str, Exchange::PairingState defaultValue) {
            if (str == "INITIALIZING" || str == "INITIALISING") return Exchange::PairingState::INITIALISING;
            if (str == "IDLE")      return Exchange::PairingState::IDLE;
            if (str == "SEARCHING") return Exchange::PairingState::SEARCHING;
            if (str == "PAIRING")   return Exchange::PairingState::PAIRING;
            if (str == "COMPLETE")  return Exchange::PairingState::COMPLETE;
            if (str == "FAILED")    return Exchange::PairingState::FAILED;
            return defaultValue;
        }

        // --- IRProgState: ctrlm sends uppercase strings ---
        template <>
        Exchange::IRProgState stringToEnum<Exchange::IRProgState>(const string& str, Exchange::IRProgState defaultValue) {
            if (str == "IDLE")     return Exchange::IRProgState::IDLE;
            if (str == "WAITING")  return Exchange::IRProgState::WAITING;
            if (str == "COMPLETE") return Exchange::IRProgState::COMPLETE;
            if (str == "FAILED")   return Exchange::IRProgState::FAILED;
            return defaultValue;
        }

        // --- FirmwareUpdateState: 1:1 mapping to ctrlm's ctrlm_rcu_upgrade_state_t strings ---
        template <>
        Exchange::FirmwareUpdateState stringToEnum<Exchange::FirmwareUpdateState>(const string& str, Exchange::FirmwareUpdateState defaultValue) {
            if (str == "success")  return Exchange::FirmwareUpdateState::SUCCESS;
            if (str == "idle")     return Exchange::FirmwareUpdateState::IDLE;
            if (str == "pending")  return Exchange::FirmwareUpdateState::PENDING;
            if (str == "canceled") return Exchange::FirmwareUpdateState::CANCELED;
            if (str == "retrying") return Exchange::FirmwareUpdateState::RETRYING;
            if (str == "error")    return Exchange::FirmwareUpdateState::ERROR;
            if (str == "invalid")  return Exchange::FirmwareUpdateState::INVALID;
            return defaultValue;
        }
    } // anonymous namespace

    SERVICE_REGISTRATION(RemoteControlImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

    Core::CriticalSection RemoteControlImplementation::_instanceLock;
    RemoteControlImplementation* RemoteControlImplementation::_instance = nullptr;

    RemoteControlImplementation::RemoteControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
        , _handlersRegistered(0)
    {
        _instanceLock.Lock();
        _instance = this;
        _instanceLock.Unlock();
    }

    RemoteControlImplementation::~RemoteControlImplementation()
    {
        _instanceLock.Lock();
        _instance = nullptr;
        _instanceLock.Unlock();

        DeinitializeIARM();

        // Release any still-registered notification observers.
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            if (notification != nullptr) {
                 notification->Release();
            }
        }
        _notifications.clear();
        _adminLock.Unlock();

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

        if (InitializeIARM() == false) {
            LOGERR("Failed to initialize IARM for RemoteControlImplementation, configuration will fail");
            _service->Release();
            _service = nullptr;
            return Core::ERROR_GENERAL;
        }
        if (Utils::IARM::isConnected() == false) {
            LOGERR("Failed to initialize IARM for RemoteControlImplementation, configuration will fail");
            DeinitializeIARM();
            _service->Release();
            _service = nullptr;
            return Core::ERROR_GENERAL;
        }
        return Core::ERROR_NONE;
    }

    // ─── INotification management ───

    Core::hresult RemoteControlImplementation::Register(Exchange::IRemoteControl::INotification* notification)
    {
        if (notification == nullptr) {
            return Core::ERROR_BAD_REQUEST;
        }

        _adminLock.Lock();
        const auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it == _notifications.end()) {
            notification->AddRef();
            _notifications.push_back(notification);
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::Unregister(const Exchange::IRemoteControl::INotification* notification)
    {
        if (notification == nullptr) {
            return Core::ERROR_BAD_REQUEST;
        }

        _adminLock.Lock();
        const auto it = std::find_if(_notifications.begin(), _notifications.end(), [notification](const Exchange::IRemoteControl::INotification* current) {
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

    bool RemoteControlImplementation::InitializeIARM()
    {
        const bool alreadyConnected = Utils::IARM::isConnected();
        if (Utils::IARM::init()) {
            _hasOwnProcess = !alreadyConnected;
            IARM_Result_t res;
#define RC_REGISTER(EVENT) \
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, EVENT, remoteEventHandler) ); \
            if (res != IARM_RESULT_SUCCESS) { \
                LOGERR("Failed to register IARM event handler for " #EVENT ", rolling back."); \
                DeinitializeIARM(); \
                return false; \
            } \
            _handlersRegistered++;
            RC_REGISTER(CTRLM_RCU_IARM_EVENT_RCU_STATUS)
            RC_REGISTER(CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS)
            RC_REGISTER(CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS)
            RC_REGISTER(CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE)
            RC_REGISTER(CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT)
#undef RC_REGISTER
        } else {
            _hasOwnProcess = false;
            return false;
        }
        return true;
    }

    void RemoteControlImplementation::DeinitializeIARM()
    {
        if (_handlersRegistered > 0) {
            // Handlers are registered in order; remove only those that were successfully registered, in reverse.
            // The registration order is:
            //   1: CTRLM_RCU_IARM_EVENT_RCU_STATUS
            //   2: CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS
            //   3: CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS
            //   4: CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE
            //   5: CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT
            IARM_Result_t res; // Used in IARM_CHECK macro calls below
            if (_handlersRegistered >= 5) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RF4CE_PAIRING_WINDOW_TIMEOUT, remoteEventHandler) ); }
            if (_handlersRegistered >= 4) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_CONFIGURATION_COMPLETE, remoteEventHandler) ); }
            if (_handlersRegistered >= 3) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_VALIDATION_STATUS, remoteEventHandler) ); }
            if (_handlersRegistered >= 2) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_FIRMWARE_UPDATE_PROGRESS, remoteEventHandler) ); }
            if (_handlersRegistered >= 1) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_RCU_IARM_EVENT_RCU_STATUS, remoteEventHandler) ); }
            _handlersRegistered = 0;
        }

        if (_hasOwnProcess) {
            IARM_Result_t res; // Used in IARM_CHECK macro calls below
            IARM_CHECK( IARM_Bus_Disconnect() );
            IARM_CHECK( IARM_Bus_Term() );
            _hasOwnProcess = false;
        }
    }

    // ─── IARM event dispatching ───

    void RemoteControlImplementation::remoteEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len)
    {
        _instanceLock.Lock();
        RemoteControlImplementation* instance = _instance;
        _instanceLock.Unlock();

        if (instance != nullptr) {
            instance->iarmEventHandler(owner, eventId, data, len);
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

    std::vector<Exchange::IRemoteControl::INotification*> RemoteControlImplementation::ObserverSnapshot()
    {
        std::vector<Exchange::IRemoteControl::INotification*> observers;

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        return observers;
    }

    void RemoteControlImplementation::ReleaseObserverSnapshot(std::vector<Exchange::IRemoteControl::INotification*>& observers)
    {
        for (auto* notification : observers) {
            notification->Release();
        }
        observers.clear();
    }

    void RemoteControlImplementation::NotifyStatus(ctrlm_main_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::NetStatusData status;

        JsonObject statusObj;
        if (params.HasLabel("status")) {
            statusObj = params["status"].Object();
        }

        status.netType = statusObj.HasLabel("netType") ? static_cast<uint32_t>(statusObj["netType"].Number()) : (params.HasLabel("netType") ? static_cast<uint32_t>(params["netType"].Number()) : 0);
        status.pairingState = statusObj.HasLabel("pairingState") ? stringToEnum<Exchange::PairingState>(statusObj["pairingState"].String(), Exchange::PairingState::IDLE) : Exchange::PairingState::IDLE;
        status.irProgState = statusObj.HasLabel("irProgState") ? stringToEnum<Exchange::IRProgState>(statusObj["irProgState"].String(), Exchange::IRProgState::IDLE) : Exchange::IRProgState::IDLE;
        status.netTypesSupported = statusObj.HasLabel("netTypesSupported") ? jsonValueToString(statusObj["netTypesSupported"]) : "[]";
        status.remoteData = statusObj.HasLabel("remoteData") ? jsonValueToString(statusObj["remoteData"]) : "[]";

        // NetStatusData.netTypesSupported and .remoteData are @opaque with no @restrict —
        // default uint16_t SetText limit is 65535 bytes.
        checkRestrictLimit(status.netTypesSupported, 65535, "NetStatusData.netTypesSupported");
        checkRestrictLimit(status.remoteData,        65535, "NetStatusData.remoteData");

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnStatus(status);
        }

        ReleaseObserverSnapshot(observers);
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

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnValidation(status);
        }

        ReleaseObserverSnapshot(observers);
    }

    void RemoteControlImplementation::NotifyFirmwareUpdateProgress(ctrlm_main_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::FirmwareUpdateStatusData status;

        if (params.HasLabel("status")) {
            JsonObject statusObj = params["status"].Object();
            status.upgradeSessionId = statusObj.HasLabel("upgradeSessionId") ? statusObj["upgradeSessionId"].String() : "";
            status.macAddress = statusObj.HasLabel("macAddress") ? statusObj["macAddress"].String() : "";
            status.upgradeState = statusObj.HasLabel("upgradeState") ? stringToEnum<Exchange::FirmwareUpdateState>(statusObj["upgradeState"].String(), Exchange::FirmwareUpdateState::INVALID) : Exchange::FirmwareUpdateState::INVALID;
            status.percentComplete = statusObj.HasLabel("percentComplete") ? static_cast<uint32_t>(statusObj["percentComplete"].Number()) : 0;
            status.errorString = statusObj.HasLabel("errorString") ? statusObj["errorString"].String() : "";
        } else {
            status.upgradeSessionId = "";
            status.macAddress = "";
            status.upgradeState = Exchange::FirmwareUpdateState::INVALID;
            status.percentComplete = 0;
            status.errorString = "";
        }

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnFirmwareUpdateProgress(status);
        }

        ReleaseObserverSnapshot(observers);
    }

    // ─── IARM call helper ───
    // Encapsulates the boilerplate: allocate ctrlm_main_iarm_call_json_t, copy JSON payload, make the IARM bus call, parse the JSON result.
    // Returns Core::ERROR_NONE on success, Core::ERROR_GENERAL for allocation failure, or Core::ERROR_RPC_CALL_FAILED if the IARM call itself fails
    Core::hresult RemoteControlImplementation::IARMBusCall(const string& method, const string& jsonParams, JsonObject& result, int timeoutMs)
    {
        const size_t totalsize = sizeof(ctrlm_main_iarm_call_json_t) + jsonParams.size() + 1;
        ctrlm_main_iarm_call_json_t* call = (ctrlm_main_iarm_call_json_t*)calloc(1, totalsize);

        if (call == nullptr) {
            LOGERR("ERROR - Cannot allocate IARM structure - size: %u.", (unsigned)totalsize);
            return Core::ERROR_GENERAL;
        }

        call->api_revision = CTRLM_MAIN_IARM_BUS_API_REVISION;
        const size_t len = jsonParams.copy(call->payload, jsonParams.size());
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
        LOGINFO("%s Bus Call SUCCESS response=%s", method.c_str(), call->result);
        free(call);
        return Core::ERROR_NONE;
    }

    // ─── IRemoteControl method implementations ───
    // The underlying IARM/ctrlm interface is JSON-based: the input parameters are serialized
    // to a JSON string, sent over IARM, and the result comes back as a JSON string.
    // Each method below converts its typed struct parameters into JSON, calls IARM, and
    // then parses the JSON result back into the typed response.

    Core::hresult RemoteControlImplementation::GetApiVersionNumber(Exchange::RemoteControlGetApiVersionNumberResponse& response)
    {
        response.version = API_VERSION_NUMBER_MAJOR;
        response.success = true;
        LOGINFO("response: version=%u, success=%s", response.version, response.success ? "true" : "false");
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::StartPairing(const Core::OptionalType<uint32_t>& timeout, const Core::OptionalType<bool>& screenBindEnable, const Core::OptionalType<bool>& scanEnable, Exchange::IStringIterator* const macAddressList, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: timeout=%s%u, screenBindEnable=%s%s, scanEnable=%s%s, macAddressList=%s",
                timeout.IsSet() ? "" : "<default>", timeout.IsSet() ? timeout.Value() : 0,
                screenBindEnable.IsSet() ? "" : "<default>", screenBindEnable.IsSet() ? (screenBindEnable.Value() ? "true" : "false") : "",
                scanEnable.IsSet() ? "" : "<default>", scanEnable.IsSet() ? (scanEnable.Value() ? "true" : "false") : "",
                (macAddressList != nullptr) ? "<provided>" : "<not set>");

        JsonObject params;
        if (timeout.IsSet()) {
            params["timeout"] = timeout.Value();
        }
        if (screenBindEnable.IsSet()) {
            params["screenBindEnable"] = screenBindEnable.Value();
        }
        if (scanEnable.IsSet()) {
            params["scanEnable"] = scanEnable.Value();
        }

        JsonArray macArray;
        if (macAddressList != nullptr) {
            string mac;
            while (macAddressList->Next(mac)) {
                macArray.Add(Core::JSON::Variant(mac));
            }
            if (macArray.Length() > 0) {
                params["macAddressList"] = macArray;
            }
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_START_PAIRING, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::StopPairing(const Core::OptionalType<bool>& screenBindDisable, const Core::OptionalType<bool>& scanDisable, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: screenBindDisable=%s%s, scanDisable=%s%s",
                screenBindDisable.IsSet() ? "" : "<default>", screenBindDisable.IsSet() ? (screenBindDisable.Value() ? "true" : "false") : "",
                scanDisable.IsSet() ? "" : "<default>", scanDisable.IsSet() ? (scanDisable.Value() ? "true" : "false") : "");

        JsonObject params;
        if (screenBindDisable.IsSet()) {
            params["screenBindDisable"] = screenBindDisable.Value();
        }
        if (scanDisable.IsSet()) {
            params["scanDisable"] = scanDisable.Value();
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_STOP_PAIRING, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetNetStatus(const uint32_t netType, Exchange::GetNetStatusResult& result)
    {
        LOGINFO("params: netType=%u", netType);
        JsonObject params;
        params["netType"] = netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_GET_RCU_STATUS, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            result.status.netType = netType;
            result.status.pairingState = Exchange::PairingState::IDLE;
            result.status.irProgState = Exchange::IRProgState::IDLE;
            result.status.netTypesSupported = "[]";
            result.status.remoteData = "[]";
            return Core::ERROR_NONE;
        }

        JsonObject statusObj;
        if (iarmResult.HasLabel("status")) {
            statusObj = iarmResult["status"].Object();
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        result.status.netType = statusObj.HasLabel("netType") ? static_cast<uint32_t>(statusObj["netType"].Number()) : netType;
        result.status.pairingState = statusObj.HasLabel("pairingState") ? stringToEnum<Exchange::PairingState>(statusObj["pairingState"].String(), Exchange::PairingState::IDLE) : Exchange::PairingState::IDLE;
        result.status.irProgState = statusObj.HasLabel("irProgState") ? stringToEnum<Exchange::IRProgState>(statusObj["irProgState"].String(), Exchange::IRProgState::IDLE) : Exchange::IRProgState::IDLE;
        result.status.netTypesSupported = statusObj.HasLabel("netTypesSupported") ? jsonValueToString(statusObj["netTypesSupported"]) : "[]";
        result.status.remoteData = statusObj.HasLabel("remoteData") ? jsonValueToString(statusObj["remoteData"]) : "[]";

        // NetStatusData.netTypesSupported and .remoteData are @opaque with no @restrict —
        // default uint16_t SetText limit is 65535 bytes.
        checkRestrictLimit(result.status.netTypesSupported, 65535, "NetStatusData.netTypesSupported");
        checkRestrictLimit(result.status.remoteData,        65535, "NetStatusData.remoteData");

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetIRDBManufacturers(Exchange::AVDevType& avDevType, const string& manufacturer, bool& success, Exchange::IStringIterator*& manufacturers)
    {
        LOGINFO("params: avDevType=%s, manufacturer=%s",
                enumToString(avDevType),
                manufacturer.empty() ? "<empty>" : manufacturer.c_str());
        if (isValidRequestEnum(avDevType) == false) {
            LOGERR("GetIRDBManufacturers requires avDevType.");
            success = false;
            manufacturers = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        if (manufacturer.empty()) {
            LOGERR("GetIRDBManufacturers requires a non-empty manufacturer parameter.");
            success = false;
            manufacturers = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["avDevType"] = enumToString(avDevType);
        params["manufacturer"] = manufacturer;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_MANUFACTURERS, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            manufacturers = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        avDevType = result.HasLabel("avDevType") ? stringToEnum<Exchange::AVDevType>(result["avDevType"].String(), avDevType) : avDevType;
        success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> mfrsList;
        if (result.HasLabel("manufacturers")) {
            auto arr = result["manufacturers"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                mfrsList.push_back(arr[i].String());
            }
        }
        manufacturers = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(mfrsList);

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetIRDBModels(Exchange::AVDevType& avDevType, string& manufacturer, const string& model, bool& success, Exchange::IStringIterator*& models)
    {
        LOGINFO("params: avDevType=%s, manufacturer=%s, model=%s",
                enumToString(avDevType),
                manufacturer.empty() ? "<empty>" : manufacturer.c_str(),
                model.empty() ? "<empty>" : model.c_str());
        if (isValidRequestEnum(avDevType) == false) {
            LOGERR("GetIRDBModels requires avDevType.");
            success = false;
            models = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["avDevType"] = enumToString(avDevType);
        params["manufacturer"] = manufacturer;
        params["model"] = model;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_MODELS, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            models = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        avDevType = result.HasLabel("avDevType") ? stringToEnum<Exchange::AVDevType>(result["avDevType"].String(), avDevType) : avDevType;
        success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> mdls;
        if (result.HasLabel("models")) {
            auto arr = result["models"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                mdls.push_back(arr[i].String());
            }
        }
        models = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(mdls);

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetIRCodesByAutoLookup(const uint32_t netType, string& tvManufacturer, string& tvModel, string& avrManufacturer, string& avrModel, bool& success, Exchange::IStringIterator*& tvCodes, Exchange::IStringIterator*& avrCodes)
    {
        LOGINFO("params: netType=%u", netType);
        JsonObject params;
        params["netType"] = netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_AUTO_LOOKUP, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            tvManufacturer.clear();
            tvModel.clear();
            avrManufacturer.clear();
            avrModel.clear();
            tvCodes = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            avrCodes = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
        }

        tvManufacturer = result.HasLabel("tvManufacturer") ? result["tvManufacturer"].String() : "";
        tvModel = result.HasLabel("tvModel") ? result["tvModel"].String() : "";
        avrManufacturer = result.HasLabel("avrManufacturer") ? result["avrManufacturer"].String() : "";
        avrModel = result.HasLabel("avrModel") ? result["avrModel"].String() : "";
        success = result.HasLabel("success") ? result["success"].Boolean() : false;

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

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetIRCodesByNames(Exchange::AVDevType& avDevType, string& manufacturer, string& model, bool& success, string& codes)
    {
        LOGINFO("params: avDevType=%s, manufacturer=%s, model=%s",
                enumToString(avDevType),
                manufacturer.empty() ? "<empty>" : manufacturer.c_str(),
                model.empty() ? "<empty>" : model.c_str());
        if (isValidRequestEnum(avDevType) == false) {
            LOGERR("GetIRCodesByNames requires avDevType.");
            success = false;
            codes = "[]";
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["avDevType"] = enumToString(avDevType);
        params["manufacturer"] = manufacturer;
        params["model"] = model;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_CODES, jsonParams, result, IARM_IRDB_CALLS_TIMEOUT);
        string resultStr;
        result.ToString(resultStr);
        LOGINFO("IARM response for GetIRCodesByNames: %s", resultStr.c_str());
        if (callResult != Core::ERROR_NONE) {
            success = false;
            codes = "[]";
            return Core::ERROR_NONE;
        }

        // Always set all mandatory output parameters
        avDevType = result.HasLabel("avDevType") ? stringToEnum<Exchange::AVDevType>(result["avDevType"].String(), avDevType) : avDevType;
        manufacturer = result.HasLabel("manufacturer") ? result["manufacturer"].String() : manufacturer;
        model = result.HasLabel("model") ? result["model"].String() : model;
        success = result.HasLabel("success") ? result["success"].Boolean() : false;

        JsonArray codesArray;
        if (result.HasLabel("codes")) {
            auto arr = result["codes"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                codesArray.Add(Core::JSON::Variant(arr[i].String()));
                LOGINFO("IR code: %s", arr[i].String().c_str());
            }
        }
        codesArray.ToString(codes);

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::SetIRCode(const uint32_t remoteId, const uint32_t netType, const Exchange::AVDevType avDevType, const string& code, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: remoteId=%u, netType=%u, avDevType=%s, code=%s",
                remoteId, netType,
                enumToString(avDevType),
                code.c_str());
        if (isValidRequestEnum(avDevType) == false) {
            LOGERR("SetIRCode requires avDevType.");
            result.success = false;
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["remoteId"] = remoteId;
        params["netType"] = netType;
        params["avDevType"] = enumToString(avDevType);
        if (!code.empty()) params["code"] = code;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_SET_CODE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::ClearIRCodes(const uint32_t remoteId, const uint32_t netType, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: remoteId=%u, netType=%u", remoteId, netType);
        JsonObject params;
        params["remoteId"] = remoteId;
        params["netType"] = netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_CLEAR_CODE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::GetLastKeypressSource(Exchange::GetLastKeypressSourceResponse& response)
    {
        LOGINFO("params={}");
        JsonObject params;
        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_LAST_KEYPRESS_GET, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            response.success = false;
            return Core::ERROR_NONE;
        }

        response.controllerId = result.HasLabel("controllerId") ? static_cast<uint32_t>(result["controllerId"].Number()) : 0;
        response.timestamp = result.HasLabel("timestamp") ? static_cast<uint64_t>(result["timestamp"].Number()) : 0;
        response.sourceName = result.HasLabel("sourceName") ? result["sourceName"].String() : "";
        response.sourceType = result.HasLabel("sourceType") ? result["sourceType"].String() : "";
        response.sourceKeyCode = result.HasLabel("sourceKeyCode") ? static_cast<uint32_t>(result["sourceKeyCode"].Number()) : 0;
        response.bIsScreenBindMode = result.HasLabel("bIsScreenBindMode") ? result["bIsScreenBindMode"].Boolean() : false;
        response.remoteKeypadConfig = result.HasLabel("remoteKeypadConfig") ? static_cast<uint32_t>(result["remoteKeypadConfig"].Number()) : 0;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::ConfigureWakeupKeys(const Exchange::WakeupConfig wakeupConfig, const Core::OptionalType<string>& customKeys, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: wakeupConfig=%s, customKeys=%s",
                enumToString(wakeupConfig),
                customKeys.IsSet() ? customKeys.Value().c_str() : "<not set>");
        if (isValidRequestEnum(wakeupConfig) == false) {
            LOGERR("ConfigureWakeupKeys requires wakeupConfig.");
            result.success = false;
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["wakeupConfig"] = enumToString(wakeupConfig);
        if (customKeys.IsSet()) {
            params["customKeys"] = customKeys.Value();
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_WRITE_RCU_WAKEUP_CONFIG, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::InitializeIRDB(const uint32_t netType, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: netType=%u", netType);
        JsonObject params;
        params["netType"] = netType;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_IR_INITIALIZE, jsonParams, iarmResult, IARM_IRDB_CALLS_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::FindMyRemote(const Exchange::FindMyRemoteLevel level, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: level=%s", enumToString(level));
        if (isValidRequestEnum(level) == false) {
            LOGERR("FindMyRemote requires level.");
            result.success = false;
            return Core::ERROR_NONE;
        }

        JsonObject params;
        params["level"] = enumToString(level);

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_FIND_MY_REMOTE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::FactoryReset(Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params={}");
        JsonObject params;
        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_FACTORY_RESET, jsonParams, iarmResult, IARM_FACTORY_RESET_TIMEOUT);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::Unpair(Exchange::RemoteControlSuccessResult& result, Exchange::IStringIterator* const macAddressList)
    {
        LOGINFO("params: macAddressList=%s",
                (macAddressList != nullptr) ? "<provided>" : "<not set>");
        JsonObject params;

        if (macAddressList != nullptr) {
            JsonArray macArray;
            string mac;
            while (macAddressList->Next(mac)) {
                macArray.Add(Core::JSON::Variant(mac));
            }
            params["macAddressList"] = macArray;
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_UNPAIR, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::StartFirmwareUpdate(const string& macAddress, const string& fileName, const string& fileType, const uint32_t percentIncrement, bool& success, Exchange::IStringIterator*& sessionIdList)
    {
        LOGINFO("params: macAddress=%s, fileName=%s, fileType=%s, percentIncrement=%u",
                macAddress.c_str(), fileName.c_str(), fileType.c_str(), percentIncrement);
        sessionIdList = nullptr;

        JsonObject params;
        if (!macAddress.empty()) params["macAddress"] = macAddress;
        if (!fileName.empty()) params["fileName"] = fileName;
        if (!fileType.empty()) params["fileType"] = fileType;
        if (percentIncrement != 0) params["percentIncrement"] = percentIncrement;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_START_FIRMWARE_UPDATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            sessionIdList = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(std::list<string>{});
            return Core::ERROR_NONE;
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

        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::CancelFirmwareUpdate(const string& sessionId, Exchange::RemoteControlSuccessResult& result)
    {
        LOGINFO("params: sessionId=%s", sessionId.c_str());
        JsonObject params;
        params["sessionId"] = sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_CANCEL_FIRMWARE_UPDATE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult["success"].Boolean();
        return Core::ERROR_NONE;
    }

    Core::hresult RemoteControlImplementation::StatusFirmwareUpdate(const string& sessionId, Exchange::StatusFirmwareUpdateResponse& response)
    {
        LOGINFO("params: sessionId=%s", sessionId.c_str());
        JsonObject params;
        params["sessionId"] = sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_MAIN_IARM_CALL_STATUS_FIRMWARE_UPDATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            response.status.upgradeSessionId = sessionId;
            response.status.macAddress.clear();
            response.status.upgradeState = Exchange::FirmwareUpdateState::INVALID;
            response.status.percentComplete = 0;
            response.status.errorString.clear();
            response.success = false;
            return Core::ERROR_NONE;
        }

        // ctrlm nests firmware fields under "status"; "success" is at the top level
        JsonObject statusObj;
        if (result.HasLabel("status")) {
            statusObj = result["status"].Object();
        }

        response.status.upgradeSessionId = statusObj.HasLabel("upgradeSessionId") ? statusObj["upgradeSessionId"].String() : sessionId;
        response.status.macAddress = statusObj.HasLabel("macAddress") ? statusObj["macAddress"].String() : "";
        response.status.upgradeState = statusObj.HasLabel("upgradeState") ? stringToEnum<Exchange::FirmwareUpdateState>(statusObj["upgradeState"].String(), Exchange::FirmwareUpdateState::INVALID) : Exchange::FirmwareUpdateState::INVALID;
        response.status.percentComplete = statusObj.HasLabel("percentComplete") ? static_cast<uint32_t>(statusObj["percentComplete"].Number()) : 0;
        response.status.errorString = statusObj.HasLabel("errorString") ? statusObj["errorString"].String() : "";
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        return Core::ERROR_NONE;
    }

} // namespace Plugin
} // namespace WPEFramework
