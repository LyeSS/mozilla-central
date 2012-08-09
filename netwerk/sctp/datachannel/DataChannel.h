/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NETWERK_SCTP_DATACHANNEL_DATACHANNEL_H_
#define NETWERK_SCTP_DATACHANNEL_DATACHANNEL_H_

#include <string>
#include "nsISupports.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsTArray.h"
#include "mozilla/Mutex.h"
#include "DataChannelProtocol.h"

extern "C" {
  struct socket;
  struct sctp_rcvinfo;
}

namespace mozilla {

class DTLSConnection;
class DataChannelConnection;
class DataChannel;
class DataChannelOnMessageAvailable;

// Implemented by consumers of a Channel to receive messages.
// Can't nest it in DataChannelConnection because C++ doesn't allow forward
// refs to embedded classes
class DataChannelListener {
public:
  virtual ~DataChannelListener() {}

  // Called when a DOMString message is received.
  virtual nsresult OnMessageAvailable(nsISupports *aContext,
                                  const nsACString& message) = 0;

  // Called when a binary message is received.
  virtual nsresult OnBinaryMessageAvailable(nsISupports *aContext,
                                        const nsACString& message) = 0;

  // Called when the channel is connected
  virtual nsresult OnChannelConnected(nsISupports *aContext) = 0;

  // Called when the channel is closed
  virtual nsresult OnChannelClosed(nsISupports *aContext) = 0;
};


// One per PeerConnection
class DataChannelConnection {
public:

  class DataConnectionListener {
  public:
    virtual ~DataConnectionListener() {}

    // Called when a the connection is open
    virtual void OnConnection() = 0;

    // Called when a the connection is lost/closed
    virtual void OnClosedConnection() = 0;

    // Called when a new DataChannel has been opened by the other side.
    virtual void OnDataChannel(DataChannel *channel) = 0;
  };

  DataChannelConnection(DataConnectionListener *listener);
  virtual ~DataChannelConnection() {} // XXX need cleanup code for SCTP sockets

  bool Init(unsigned short port /* XXX DTLSConnection &tunnel*/);

  // XXX These will need to be replaced with something better
  // They block; they require something to decide on listener/connector,
  // etc.  Apparently SCTP associations can be simultaneously opened from
  // each end and the stack resolves it.
  bool Listen(unsigned short port);
  bool Connect(const char *addr, unsigned short port);

  typedef enum {
    RELIABLE=0,
    RELIABLE_STREAM = 1,
    UNRELIABLE = 2,
    PARTIAL_RELIABLE_REXMIT = 3,
    PARTIAL_RELIABLE_TIMED = 4
  } Type;
    
  DataChannel *Open(/* const std::wstring& channel_label,*/
                    Type type, bool inOrder, 
                    PRUint32 prValue, DataChannelListener *aListener,
                    nsISupports *aContext);

  void Close(PRUint16 stream);
  void CloseAll();

  PRInt32 SendMsgCommon(PRUint16 stream, const nsACString &aMsg, bool isBinary);
  PRInt32 SendMsg(PRUint16 stream, const nsACString &aMsg)
    {
      return SendMsgCommon(stream, aMsg, false);
    }
  PRInt32 SendBinaryMsg(PRUint16 stream, const nsACString &aMsg)
    {
      return SendMsgCommon(stream, aMsg, true);
    }

  // Called on data reception from the SCTP library
  // must(?) be public so my c->c++ tramploine can call it
  int ReceiveCallback(struct socket* sock, void *data, size_t datalen, 
                      struct sctp_rcvinfo rcv, PRInt32 flags);

  // Find out state
  enum {
    CONNECTING = 0U,
    OPEN = 1U,
    CLOSING = 2U,
    CLOSED = 3U
  };
  PRUint16 GetReadyState() { return mState; }

  friend class DataChannel;
  Mutex  mLock;

  // XXX I'd like this to be protected or private...
  DataConnectionListener *mListener;

private:
  DataChannel* FindChannelByStreamIn(PRUint16 streamIn);
  DataChannel* FindChannelByStreamOut(PRUint16 streamOut);
  PRUint16 FindFreeStreamOut();
  bool RequestMoreStreamsOut();
  PRInt32 SendControlMessage(void *msg, PRUint32 len, PRUint16 streamOut);
  PRInt32 SendOpenRequestMessage(PRUint16 streamOut, bool unordered, PRUint16 prPolicy, PRUint32 prValue);
  PRInt32 SendOpenResponseMessage(PRUint16 streamOut, PRUint16 streamIn);
  PRInt32 SendOpenAckMessage(PRUint16 streamOut);
  void SendDeferredMessages();
  void SendOutgoingStreamReset();
  void ResetOutgoingStream(PRUint16 streamOut);
  void HandleOpenRequestMessage(const struct rtcweb_datachannel_open_request *req,
                                size_t length,
                                PRUint16 streamIn);
  void HandleOpenResponseMessage(const struct rtcweb_datachannel_open_response *rsp,
                                 size_t length, PRUint16 streamIn);
  void HandleOpenAckMessage(const struct rtcweb_datachannel_ack *ack,
                            size_t length, PRUint16 streamIn);
  void HandleUnknownMessage(PRUint32 ppid, size_t length, PRUint16 streamIn);
  void HandleDataMessage(PRUint32 ppid, const char *buffer, size_t length, PRUint16 streamIn);
  void HandleMessage(char *buffer, size_t length, PRUint32 ppid, PRUint16 streamIn);
  void HandleAssociationChangeEvent(const struct sctp_assoc_change *sac);
  void HandlePeerAddressChangeEvent(const struct sctp_paddr_change *spc);
  void HandleRemoteErrorEvent(const struct sctp_remote_error *sre);
  void HandleShutdownEvent(const struct sctp_shutdown_event *sse);
  void HandleAdaptationIndication(const struct sctp_adaptation_event *sai);
  void HandleSendFailedEvent(const struct sctp_send_failed_event *ssfe);
  void HandleStreamResetEvent(const struct sctp_stream_reset_event *strrst);
  void HandleStreamChangeEvent(const struct sctp_stream_change_event *strchg);
  void HandleNotification(const union sctp_notification *notif, size_t n);

  // NOTE: while these arrays will auto-expand, increases in the number of
  // channels available from the stack must be negotiated!
  nsAutoTArray<DataChannel*,16> mStreamsOut;
  nsAutoTArray<DataChannel*,16> mStreamsIn;

  // Streams pending reset
  nsAutoTArray<PRUint16,4> mStreamsResetting;

  struct socket *mMasterSocket;
  struct socket *mSocket;
  PRUint16 mNumChannels;
  PRUint16 mState;
};

class DataChannel {
public:
  enum {
    CONNECTING = 0U,
    OPEN = 1U,
    CLOSING = 2U,
    CLOSED = 3U
  };

  DataChannel(DataChannelConnection *connection,
              PRUint16 streamOut, PRUint16 streamIn, 
              PRUint16 state,
              PRUint16 policy, PRUint32 value,
              PRUint32 flags,
              DataChannelListener *aListener,
              nsISupports *aContext) : 
    mListener(aListener), mConnection(connection), mState(state),
    mStreamOut(streamOut), mStreamIn(streamIn),
    mPrPolicy(policy), mPrValue(value),
    mFlags(0), mContext(aContext)
    {
      NS_ASSERTION(mConnection,"NULL connection");
    }

  ~DataChannel()
    {
      Close();
    }

  // Close this DataChannel.  Can be called multiple times.
  void Close() 
    { 
      if (mState == CLOSING || mState == CLOSED ||
          mStreamOut == INVALID_STREAM) {
        return;
      }
      mState = CLOSING;
      mConnection->Close(mStreamOut);
      mStreamOut = INVALID_STREAM;
      mStreamIn  = INVALID_STREAM;
    }

  // Set the listener (especially for channels created from the other side)
  void SetListener(DataChannelListener *aListener, nsISupports *aContext)
    { mContext = aContext; mListener = aListener; } // XXX Locking?

  // Send a string
  bool SendMsg(const nsACString &aMsg)
    {
      if (mStreamOut != INVALID_STREAM)
        return (mConnection->SendMsg(mStreamOut, aMsg) > 0);
      else
        return false;
    }

  // Send a binary message (blob or TypedArray)
  bool SendBinaryMsg(const nsACString &aMsg)
    {
      if (mStreamOut != INVALID_STREAM)
        return (mConnection->SendBinaryMsg(mStreamOut, aMsg) > 0);
      else
        return false;
    }

  // XXX I don't think we need SendBinaryStream()

  // Amount of data buffered to send
  PRUint32 GetBufferedAmount() { return 0; /* XXX */ }

  // Find out state
  PRUint16 GetReadyState() { return mState; }
  void SetReadyState(PRUint16 aState) { mState = aState; }

  // XXX I'd like this to be protected or private...
  DataChannelListener *mListener;

private:
  friend class DataChannelOnMessageAvailable;
  friend class DataChannelConnection;

  DataChannelConnection *mConnection; // XXX nsRefPtr<DataChannelConnection> mConnection;
  PRUint16 mState;
  PRUint16 mStreamOut;
  PRUint16 mStreamIn;
  PRUint16 mPrPolicy;
  PRUint32 mPrValue;
  PRUint32 mFlags;
  PRUint32 mId;
  nsCOMPtr<nsISupports> mContext;
};

// used to dispatch notifications of incoming data to the main thread
// Patterned on CallOnMessageAvailable in WebSockets
class DataChannelOnMessageAvailable : public nsRunnable
{
public:
  enum {
    ON_CONNECTION,
    ON_DISCONNECTED,
    ON_CHANNEL_CREATED,
    ON_CHANNEL_OPEN,
    ON_CHANNEL_CLOSED,
    ON_DATA,
  };  /* types */

  DataChannelOnMessageAvailable(PRInt32     aType,
                                DataChannelConnection *aConnection,
                                DataChannel *aChannel,
                                nsCString   &aData,  // XXX this causes inefficiency
                                PRInt32     aLen)
    : mType(aType),
      mChannel(aChannel),
      mConnection(aConnection), 
      mData(aData),
      mLen(aLen) {}

  DataChannelOnMessageAvailable(PRInt32     aType,
                                DataChannel *aChannel)
    : mType(aType),
      mChannel(aChannel) {}
  // XXX is it safe to leave mData/mLen uninitialized?  This should only be
  // used for notifications that don't use them, but I'd like more
  // bulletproof compile-time checking.

  DataChannelOnMessageAvailable(PRInt32     aType,
                                DataChannelConnection *aConnection,
                                DataChannel *aChannel)
    : mType(aType),
      mChannel(aChannel),
      mConnection(aConnection) {}

  NS_IMETHOD Run()
  {
    printf("OnMessage: mChannel %p mConnection %p\n",mChannel,mConnection);
    switch (mType) {
      case ON_DATA:
        printf("OnMessage: ON_DATA:  mListener %p context %p, mLen %d\n",(void *)mChannel->mListener,(void*) mChannel->mContext,mLen);
        if (mLen < 0) {
          mChannel->mListener->OnMessageAvailable(mChannel->mContext, mData);
        } else {
          mChannel->mListener->OnBinaryMessageAvailable(mChannel->mContext, mData);
        }
        break;
      case ON_CHANNEL_OPEN:
        mChannel->mListener->OnChannelConnected(mChannel->mContext);
        break;
      case ON_CHANNEL_CLOSED:
        mChannel->mListener->OnChannelClosed(mChannel->mContext);
        break;
      case ON_CHANNEL_CREATED:
        mConnection->mListener->OnDataChannel(mChannel);
        break;
      case ON_CONNECTION:
        mConnection->mListener->OnConnection();
        break;
      case ON_DISCONNECTED:
        mConnection->mListener->OnClosedConnection();
        break;
    }
    return NS_OK;
  }

private:
  ~DataChannelOnMessageAvailable() {}

  PRInt32                           mType;
  // XXX should use union
  // XXX these need to be refptrs so as to hold them open until it's delivered
  DataChannel                       *mChannel;    // XXX careful of ownership! 
  DataChannelConnection             *mConnection; // XXX careful of ownership! - should be nsRefPtr
  nsCString                         mData;
  PRInt32                           mLen;
};

}

#endif  // NETWERK_SCTP_DATACHANNEL_DATACHANNEL_H_
