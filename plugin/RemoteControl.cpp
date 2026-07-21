 /*
  * If not stated otherwise in this file or this component's LICENSE file the
  * following copyright and licenses apply:
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
  */

#include "RemoteControl.h"
#include "PluginVersion.h"
#include <list>

namespace WPEFramework {

    namespace {
        static Plugin::Metadata<Plugin::RemoteControl> metadata(
            // Version (Major, Minor, Patch)
            API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {}
        );
    }

namespace Plugin {

    SERVICE_REGISTRATION(RemoteControl, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

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

        if (_implementation != nullptr)
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

            if (message.empty())
            {
                const uint32_t registerResult = _implementation->Register(&_notification);
                if (registerResult != Core::ERROR_NONE)
                {
                    message = _T("RemoteControl failed to register notification handler");
                }
                else
                {
                    Exchange::JRemoteControl::Register(*this, _implementation);
                    // Override generated startPairing marshalling so JSON-RPC callers can
                    // provide top-level optional fields (for example timeout) while the
                    // COM-RPC backend still receives the internal opaque payload string.
                    Unregister("startPairing");
                    Register<JsonObject, JsonObject>("startPairing",
                        [this](const JsonObject& parameters, JsonObject& response) -> uint32_t {
                            if (_implementation == nullptr) {
                                return Core::ERROR_UNAVAILABLE;
                            }

                            string paramsStr;
                            parameters.ToString(paramsStr);
                            LOGINFO("startPairing params=%s", paramsStr.c_str());

                            JsonObject payloadObj;

                            // Preserve optionality by copying only labels that are actually present in
                            // the JSON-RPC request. Keep macAddressList separate as iterator transport.
                            JsonObject::Iterator it = parameters.Variants();
                            while (it.Next()) {
                                const string label = it.Label();
                                if (label != "macAddressList") {
                                    payloadObj[label.c_str()] = it.Current();
                                }
                            }

                            string payload;
                            payloadObj.ToString(payload);

                            Exchange::IStringIterator* macIter = nullptr;
                            std::list<string> macList;
                            if (parameters.HasLabel("macAddressList")) {
                                auto arr = parameters["macAddressList"].Array();
                                for (uint16_t i = 0; i < arr.Length(); i++) {
                                    macList.push_back(arr[i].String());
                                }
                                macIter = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(macList);
                            }

                            Exchange::RemoteControlSuccessResult result{};
                            const uint32_t hr = _implementation->StartPairing(payload, result, macIter);

                            if (macIter != nullptr) {
                                macIter->Release();
                            }

                            if (hr == Core::ERROR_NONE) {
                                response["success"] = result.success;
                            }

                            string responseStr;
                            response.ToString(responseStr);
                            LOGINFO("startPairing result: hr=%u response=%s", hr, responseStr.c_str());

                            return hr;
                        });

                    // Override generated stopPairing marshalling for the same reason:
                    // keep payload internal and accept only top-level JSON-RPC params.
                    Unregister("stopPairing");
                    Register<JsonObject, JsonObject>("stopPairing",
                        [this](const JsonObject& parameters, JsonObject& response) -> uint32_t {
                            if (_implementation == nullptr) {
                                return Core::ERROR_UNAVAILABLE;
                            }

                            string paramsStr;
                            parameters.ToString(paramsStr);
                            LOGINFO("stopPairing params=%s", paramsStr.c_str());

                            string payload;
                            parameters.ToString(payload);
                            Exchange::RemoteControlSuccessResult result{};
                            const uint32_t hr = _implementation->StopPairing(payload, result);

                            if (hr == Core::ERROR_NONE) {
                                response["success"] = result.success;
                            }

                            string responseStr;
                            response.ToString(responseStr);
                            LOGINFO("stopPairing result: hr=%u response=%s", hr, responseStr.c_str());

                            return hr;
                        });
                }
            }
        }
        else
        {
            message = _T("RemoteControl could not be instantiated");
        }

        if (!message.empty())
        {
            // Roll back any resources acquired during a failed initialization.
             if (_implementation != nullptr)
             {
                 if (_configure != nullptr)
                 {
                     _configure->Release();
                     _configure = nullptr;
                 }
                 RPC::IRemoteConnection* connection = service->RemoteConnection(_connectionId);
                 const uint32_t refCount = _implementation->Release();
                 _implementation = nullptr;
                 if (refCount != 0)
                 {
                     LOGWARN("RemoteControl implementation refCount after Release is %u during initialization rollback (expected 0); proceeding with remote connection termination.", refCount);
                 }
                 if (connection != nullptr) {
                     connection->Terminate();
                     connection->Release();
                 }
             }
             _connectionId = 0;
             _service->Unregister(&_connectionNotification);
             _service->Release();
             _service = nullptr;
        }

        return message;
    }

    void RemoteControl::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        _service->Unregister(&_connectionNotification);

        if (_implementation != nullptr)
        {
            _implementation->Unregister(&_notification);
            Exchange::JRemoteControl::Unregister(*this);
            Unregister("startPairing");
            Unregister("stopPairing");

            if (_configure != nullptr) {
                _configure->Release();
                _configure = nullptr;
            }

            RPC::IRemoteConnection* connection = service->RemoteConnection(_connectionId);
            const uint32_t refCount = _implementation->Release();
            _implementation = nullptr;
            if (refCount != 0)
            {
                LOGWARN("RemoteControl implementation refCount after Release is %u during shutdown (expected 0); proceeding with remote connection termination.", refCount);
            }

            if (connection != nullptr)
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
        ASSERT(connection != nullptr);

        if ((connection->Id() == _connectionId))
        {
            if (_service != nullptr)
            {
                _service->AddRef();
                Core::IWorkerPool::Instance().Submit(
                    PluginHost::IShell::Job::Create(_service,
                        PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
                _service->Release();
            }
        }
    }

} // namespace Plugin
} // namespace WPEFramework
