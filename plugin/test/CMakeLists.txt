###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###

set(PLUGIN_NAME remoteControlTestClient)

find_package(GTest REQUIRED)

add_executable(${PLUGIN_NAME} remoteControlTestClient.cpp)

set_target_properties(${PLUGIN_NAME} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    )

target_link_libraries(${PLUGIN_NAME} PRIVATE ${NAMESPACE}Core::${NAMESPACE}Core GTest::gtest_main GTest::gtest)

if (USE_THUNDER_R4)
target_link_libraries(${PLUGIN_NAME} PRIVATE ${NAMESPACE}COM::${NAMESPACE}COM  ${NAMESPACE}WebSocket::${NAMESPACE}WebSocket)
else ()
target_link_libraries(${PLUGIN_NAME} PRIVATE ${NAMESPACE}Protocols::${NAMESPACE}Protocols)
endif (USE_THUNDER_R4)

find_library(TESTMOCKLIB_LIBRARIES NAMES TestMocklib)
if (TESTMOCKLIB_LIBRARIES)
    message ("Found mock library - ${TESTMOCKLIB_LIBRARIES}")
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${TESTMOCKLIB_LIBRARIES})
else (TESTMOCKLIB_LIBRARIES)
    message ("Require ${TESTMOCKLIB_LIBRARIES} library")
endif (TESTMOCKLIB_LIBRARIES)


install(TARGETS ${PLUGIN_NAME} DESTINATION bin)
