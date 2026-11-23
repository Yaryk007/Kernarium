// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for linux/vm_sockets.h (see prelude.h).
#pragma once
#include <sys/socket.h>

#define VMADDR_CID_ANY (-1U)
struct sockaddr_vm {
    unsigned short svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
    unsigned char svm_zero[4];
};
