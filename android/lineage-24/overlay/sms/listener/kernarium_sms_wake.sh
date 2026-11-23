#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Starts the process of the text message service. The holder of vsock port 5560 starts this script through
# init when a connection from the host waits and no session runs; the service takes the listening socket
# from the holder and ends itself once the port has been quiet. The script runs as the system user, which is
# the service's own, so that the service need not be exported. The verifier requires exactly this command.
exec cmd activity start-service --user 0 -n android.kernarium.sms/.HostService >/dev/null 2>&1
