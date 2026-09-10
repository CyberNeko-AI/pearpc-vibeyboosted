/*
 * PearPC
 * slirpeth.h
 *
 * User-mode NAT ethernet tunnel backend based on libslirp
 *
 * Copyright (C) 2026 PearPC contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SLIRPETH_H__
#define __SLIRPETH_H__

#include "system/sysethtun.h"

/**
 * User-mode NAT ethernet tunnel for PearPC.
 *
 * This backend replaces the legacy TUN/TAP devices on platforms where they
 * are unavailable (e.g. modern macOS, which removed tun.kext). It uses
 * libslirp (the same engine as QEMU's "-netdev user") to provide:
 *
 *  - a built-in DHCP server (guest gets 10.0.2.15/24 by default),
 *  - ARP resolution for the virtual gateway (10.0.2.2),
 *  - a DNS proxy (10.0.2.3),
 *  - outbound TCP/UDP NAT over ordinary host sockets,
 *  - optional host->guest port forwarding (see addHostForward()).
 *
 * No root privileges or kernel drivers are required.
 */
class SlirpEthTunDevice: public EthTunDevice {
public:
    SlirpEthTunDevice();
    virtual ~SlirpEthTunDevice();

    virtual uint recvPacket(void *buf, uint size);
    virtual int waitRecvPacket();
    virtual uint sendPacket(void *buf, uint size);

    virtual uint getWriteFramePrefix();
    virtual int initDevice();
    virtual int shutdownDevice();

    virtual bool addHostForward(const char *spec);

    // internal (used by the libslirp C callbacks and the worker thread)
    struct Impl;
    Impl *d;
    void workerLoop();
    bool applyHostForward(const char *spec);
};

/* implementation in slirpeth.cc */
extern EthTunDevice *createSlirpEthernetTunnel();

#endif /* __SLIRPETH_H__ */
