# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(uml_delayed_answer_device_tests delayed_answer_device_tests.cpp)
target_link_libraries(uml_delayed_answer_device_tests PRIVATE
    UML::DelayedAnswerDevice)
uml_add_unit_test(NAME delayed-answer-device
    COMMAND uml_delayed_answer_device_tests
    LABELS vhost-user)

# A device that holds requests across a guest's suspend, behind the real
# transport. The front end it needs is the GPU tests', so their support header
# and the GPU protocol header it includes are on the path.
add_executable(uml_vhost_user_held_requests_tests held_requests_tests.cpp)
target_include_directories(uml_vhost_user_held_requests_tests BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit/gpu
    ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_link_libraries(uml_vhost_user_held_requests_tests PRIVATE
    UML::VhostUserTransport)
uml_add_unit_test(NAME vhost-user-held-requests
    COMMAND uml_vhost_user_held_requests_tests
    LABELS vhost-user)

# The server's wait for a backend request's acknowledgement, on the transport
# built with a clock the test moves.
add_executable(uml_vhost_user_backend_wait_tests backend_wait_tests.cpp)
target_include_directories(uml_vhost_user_backend_wait_tests BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit/gpu
    ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_link_libraries(uml_vhost_user_backend_wait_tests PRIVATE
    uml_vhost_user_wait_test_variant)
uml_add_unit_test(NAME vhost-user-backend-wait
    COMMAND uml_vhost_user_backend_wait_tests
    LABELS vhost-user)

# The in-process service around a device that posts work from its own thread:
# the transport blocks while it is idle, and every kind of work reaches the
# guest through the device's event descriptor alone.
add_executable(uml_vhost_user_idle_transport_tests idle_transport_tests.cpp)
target_include_directories(uml_vhost_user_idle_transport_tests BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit/gpu
    ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_link_libraries(uml_vhost_user_idle_transport_tests PRIVATE
    UML::VhostUserTransport)
uml_add_unit_test(NAME vhost-user-idle-transport
    COMMAND uml_vhost_user_idle_transport_tests
    TIMEOUT 120
    LABELS vhost-user)
