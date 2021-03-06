/*
 * Copyright (C) 2006 RobotCub Consortium
 * Authors: Paul Fitzpatrick
 * CopyPolicy: Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 */

#ifndef YARP_OS_IMPL_LOCALCARRIER_H
#define YARP_OS_IMPL_LOCALCARRIER_H

#include <yarp/os/AbstractCarrier.h>
#include <yarp/os/impl/SocketTwoWayStream.h>
#include <yarp/os/Semaphore.h>


namespace yarp {
    namespace os {
        namespace impl {
            class LocalCarrier;
            class LocalCarrierManager;
            class LocalCarrierStream;
        }
    }
}

/**
 * Coordinate ports communicating locally within a process.
 */
class yarp::os::impl::LocalCarrierManager
{
public:
    LocalCarrierManager();

    void setSender(LocalCarrier *sender);
    LocalCarrier *getReceiver();
    LocalCarrier *getSender(LocalCarrier *receiver);
    void revoke(LocalCarrier *carrier);

private:
    yarp::os::Semaphore senderMutex, receiverMutex, received;
    LocalCarrier *sender, *receiver;
};


/**
 * A stream for communicating locally within a process.
 */
class yarp::os::impl::LocalCarrierStream : public SocketTwoWayStream
{
public:
    void attach(LocalCarrier *owner, bool sender);

    virtual void interrupt() override;
    virtual void close() override;
    virtual bool isOk() override;

private:
    LocalCarrier *owner;
    bool sender;
    bool done;
};

/**
 * A carrier for communicating locally within a process.
 */
class yarp::os::impl::LocalCarrier : public AbstractCarrier
{
public:
    LocalCarrier();

    virtual ~LocalCarrier();

    virtual Carrier *create() override;

    virtual ConstString getName() override;

    virtual bool requireAck() override;
    virtual bool isConnectionless() override;
    virtual bool canEscape() override;
    virtual bool isLocal() override;
    virtual ConstString getSpecifierName();
    virtual bool checkHeader(const Bytes& header) override;
    virtual void getHeader(const Bytes& header) override;
    virtual void setParameters(const Bytes& header) override;
    virtual bool sendHeader(ConnectionState& proto) override;
    virtual bool expectExtraHeader(ConnectionState& proto) override;
    virtual bool becomeLocal(ConnectionState& proto, bool sender);
    virtual bool write(ConnectionState& proto, SizedWriter& writer) override;
    virtual bool respondToHeader(ConnectionState& proto) override;
    virtual bool expectReplyToHeader(ConnectionState& proto) override;
    virtual bool expectIndex(ConnectionState& proto) override;

    void removePeer();
    void shutdown();
    void accept(yarp::os::Portable *ref);

protected:
    bool doomed;
    yarp::os::Portable *ref;
    LocalCarrier *peer;
    yarp::os::Semaphore peerMutex;
    yarp::os::Semaphore sent, received;
    ConstString portName;

    static LocalCarrierManager manager;
};

#endif // YARP_OS_IMPL_LOCALCARRIER_H
