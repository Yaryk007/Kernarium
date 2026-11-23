# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_NETWORK_PROVIDER)
    # Each probe drives the provider's test variant from its own executable.
    set(_uml_slirp_probe_targets
        uml_slirp_network_tests
        uml_slirp_forward_tests
        uml_slirp_forward_backlog_tests
        uml_slirp_reset_peer_tests
        uml_slirp_tcp_window_tests
        uml_slirp_tcp_liveness_tests
        uml_slirp_connect_failure_tests
        uml_slirp_icmp_tests
        uml_slirp_transmit_backpressure_tests
        uml_slirp_dns_tests
        uml_slirp_dead_peer_tests
        uml_slirp_counters_tests
        uml_slirp_full_buffer_hangup_tests
        uml_slirp_host_isolation_tests
        uml_slirp_guest_mac_tests
        uml_slirp_address_reset_tests)
    set(_uml_slirp_probe_sources
        slirp_arp_probe.cpp
        slirp_forward_probe.cpp
        slirp_forward_backlog_probe.cpp
        slirp_reset_peer_probe.cpp
        slirp_tcp_window_probe.cpp
        slirp_tcp_liveness_probe.cpp
        slirp_connect_failure_probe.cpp
        slirp_icmp_probe.cpp
        slirp_transmit_backpressure_probe.cpp
        slirp_dns_probe.cpp
        slirp_dead_peer_probe.cpp
        slirp_counters_probe.cpp
        slirp_full_buffer_hangup_probe.cpp
        slirp_host_isolation_probe.cpp
        slirp_guest_mac_probe.cpp
        slirp_address_reset_probe.cpp)
    set(_uml_slirp_probe_tests
        slirp-network-arp
        slirp-network-forward-specification
        slirp-network-forward-backlog
        slirp-network-reset-peer
        slirp-network-tcp-window
        slirp-network-tcp-liveness
        slirp-network-connect-failure
        slirp-network-icmp
        slirp-network-transmit-backpressure
        slirp-network-dns
        slirp-network-dead-peer
        slirp-network-counters
        slirp-network-full-buffer-hangup
        slirp-network-host-isolation
        slirp-network-guest-mac
        slirp-network-address-reset)
    # The dead-peer probe runs five connections one after the other. The
    # full-buffer probe runs nine scenarios, each under its own 20 s alarm
    # that names a stalled one, so its limit exceeds 9 x 20 s. The address
    # reset probe waits out an 8 s hold on the system's clock up to four
    # times.
    set(_uml_slirp_probe_timeouts 30 30 30 30 30 30 30 30 30 30 60 30 240 60 60 120)
    foreach(_uml_slirp_target _uml_slirp_source _uml_slirp_test
            _uml_slirp_timeout IN ZIP_LISTS
            _uml_slirp_probe_targets
            _uml_slirp_probe_sources
            _uml_slirp_probe_tests
            _uml_slirp_probe_timeouts)
        add_executable(${_uml_slirp_target}
            ${PROJECT_SOURCE_DIR}/tests/tooling/network/${_uml_slirp_source})
        target_link_libraries(${_uml_slirp_target} PRIVATE
            uml_network_provider_test_variant
            UML::ThirdPartySlirp)
        target_include_directories(${_uml_slirp_target} PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)

        uml_add_tooling_test(
            NAME ${_uml_slirp_test}
            COMMAND ${_uml_slirp_target}
            LABELS network
            TIMEOUT ${_uml_slirp_timeout})
    endforeach()
    # The counters probe also reads a real session record.
    target_link_libraries(uml_slirp_counters_tests PRIVATE
        UML::RuntimeSessionRecord)
endif()
