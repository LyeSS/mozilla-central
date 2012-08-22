/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDOMDataChannel.h"
#include "nsIDOMFile.h"
#include "nsIJSNativeInitializer.h"
#include "nsIDOMDataChannel.h"
#include "nsIDOMMessageEvent.h"
#include "nsDOMClassInfo.h"
#include "nsDOMEventTargetHelper.h"

#include "jsval.h"

#include "nsDOMError.h"
#include "nsAutoPtr.h"
#include "nsContentUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsIScriptObjectPrincipal.h"
#include "nsJSUtils.h"
#include "nsNetUtil.h"
#include "nsDOMFile.h"

#include "DataChannel.h"

//#define LOG(x)   do { printf x; putc('\n',stdout); fflush(stdout);} while (0)
#define LOG(x)   

class nsDOMDataChannel : public nsDOMEventTargetHelper,
                         public nsIDOMDataChannel,
                         public mozilla::DataChannelListener
{
public:
  nsDOMDataChannel(mozilla::DataChannel* aDataChannel) : mDataChannel(aDataChannel),
                                                         mBinaryType(DC_BINARY_TYPE_BLOB)
  {}

  nsresult Init(nsPIDOMWindow* aDOMWindow);

  NS_DECL_ISUPPORTS_INHERITED

  NS_DECL_NSIDOMDATACHANNEL

  NS_FORWARD_NSIDOMEVENTTARGET(nsDOMEventTargetHelper::)

  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(nsDOMDataChannel,
                                           nsDOMEventTargetHelper)

  nsresult
  DoOnMessageAvailable(const nsACString& message, bool isBinary);

  virtual nsresult
  OnMessageAvailable(nsISupports* aContext, const nsACString& message);

  virtual nsresult
  OnBinaryMessageAvailable(nsISupports* aContext, const nsACString& message);

  virtual nsresult
  OnChannelConnected(nsISupports* aContext);

  virtual nsresult
  OnChannelClosed(nsISupports* aContext);

private:
  // Get msg info out of JS variable being sent (string, arraybuffer, blob)
  nsresult GetSendParams(nsIVariant *aData, nsCString &aStringOut,
                         nsCOMPtr<nsIInputStream> &aStreamOut,
                         bool &aIsBinary, PRUint32 &aOutgoingLength,
                         JSContext *aCx);

  nsresult CreateResponseBlob(const nsACString& aData, JSContext *aCx,
                              jsval &jsData);

  // Owning reference
  nsAutoPtr<mozilla::DataChannel> mDataChannel;
  nsString  mUTF16Origin;
  enum
  {
    DC_BINARY_TYPE_ARRAYBUFFER,
    DC_BINARY_TYPE_BLOB,
  } mBinaryType;

  NS_DECL_EVENT_HANDLER(open)
  NS_DECL_EVENT_HANDLER(error)
  NS_DECL_EVENT_HANDLER(close)
  NS_DECL_EVENT_HANDLER(message)

};

DOMCI_DATA(DataChannel, nsDOMDataChannel)

NS_IMPL_CYCLE_COLLECTION_CLASS(nsDOMDataChannel)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(nsDOMDataChannel,
                                                  nsDOMEventTargetHelper)
  NS_CYCLE_COLLECTION_TRAVERSE_EVENT_HANDLER(open)
  NS_CYCLE_COLLECTION_TRAVERSE_EVENT_HANDLER(error)
  NS_CYCLE_COLLECTION_TRAVERSE_EVENT_HANDLER(close)
  NS_CYCLE_COLLECTION_TRAVERSE_EVENT_HANDLER(message)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(nsDOMDataChannel,
                                                nsDOMEventTargetHelper)
  NS_CYCLE_COLLECTION_UNLINK_EVENT_HANDLER(open)
  NS_CYCLE_COLLECTION_UNLINK_EVENT_HANDLER(error)
  NS_CYCLE_COLLECTION_UNLINK_EVENT_HANDLER(close)
  NS_CYCLE_COLLECTION_UNLINK_EVENT_HANDLER(message)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_ADDREF_INHERITED(nsDOMDataChannel, nsDOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(nsDOMDataChannel, nsDOMEventTargetHelper)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(nsDOMDataChannel)
  NS_INTERFACE_MAP_ENTRY(nsIDOMDataChannel)
  NS_DOM_INTERFACE_MAP_ENTRY_CLASSINFO(DataChannel)
NS_INTERFACE_MAP_END_INHERITING(nsDOMEventTargetHelper)

nsresult
nsDOMDataChannel::Init(nsPIDOMWindow* aDOMWindow)
{
  nsresult rv;
  nsAutoString urlParam;

  nsDOMEventTargetHelper::Init();

  MOZ_ASSERT(mDataChannel);
  mDataChannel->SetListener(this, nsnull);

  // Now grovel through the objects to get a usable origin for onMessage
  nsCOMPtr<nsIScriptGlobalObject> sgo = do_QueryInterface(aDOMWindow);
  NS_ENSURE_STATE(sgo);
  nsCOMPtr<nsIScriptContext> scriptContext = sgo->GetContext();
  NS_ENSURE_STATE(scriptContext);

  nsCOMPtr<nsIScriptObjectPrincipal> scriptPrincipal(do_QueryInterface(aDOMWindow));
  NS_ENSURE_STATE(scriptPrincipal);
  nsCOMPtr<nsIPrincipal> principal = scriptPrincipal->GetPrincipal();
  NS_ENSURE_STATE(principal);

  if (aDOMWindow) {
    BindToOwner(aDOMWindow->IsOuterWindow() ?
                aDOMWindow->GetCurrentInnerWindow() : aDOMWindow);
  } else {
    BindToOwner(aDOMWindow);
  }

  // XXX any need to CheckInnerWindowCorrectness() like WebSockets?
  // It's only an issue if the PeerConnection can likewise leak, which I think it can't.
  // See bug 696085
  // Do we need to observe for window destroyed or frozen?  (same bug)

  rv = nsContentUtils::GetUTFOrigin(principal,mUTF16Origin);
  LOG(("%s: origin = %s\n",__FUNCTION__,NS_LossyConvertUTF16toASCII(mUTF16Origin).get()));
  return rv;
}

NS_IMPL_EVENT_HANDLER(nsDOMDataChannel, open)
NS_IMPL_EVENT_HANDLER(nsDOMDataChannel, error)
NS_IMPL_EVENT_HANDLER(nsDOMDataChannel, close)
NS_IMPL_EVENT_HANDLER(nsDOMDataChannel, message)

NS_IMETHODIMP
nsDOMDataChannel::GetLabel(nsAString& aLabel)
{
  mDataChannel->GetLabel(aLabel);
  return NS_OK;
}

// XXX should be GetType()
NS_IMETHODIMP
nsDOMDataChannel::GetReliable(bool* aReliable)
{
  *aReliable = true; //mDataChannel->GetType() == mozilla::DataChannelConnection::RELIABLE;
                      // With the amount of time I implemented this in
                      // it certainly isn't reliable!
  return NS_OK;
}

NS_IMETHODIMP
nsDOMDataChannel::GetReadyState(PRUint16* aReadyState)
{
  *aReadyState = mDataChannel->GetReadyState();
  return NS_OK;
}

NS_IMETHODIMP
nsDOMDataChannel::GetBufferedAmount(PRUint32* aBufferedAmount)
{
  *aBufferedAmount = mDataChannel->GetBufferedAmount();
  return NS_OK;
}

NS_IMETHODIMP
nsDOMDataChannel::GetBinaryType(nsAString& aBinaryType)
{
  switch (mBinaryType) {
  case DC_BINARY_TYPE_ARRAYBUFFER:
    aBinaryType.AssignLiteral("arraybuffer");
    break;
  case DC_BINARY_TYPE_BLOB:
    aBinaryType.AssignLiteral("blob");
    break;
  default:
    NS_ERROR("Should not happen");
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDOMDataChannel::SetBinaryType(const nsAString& aBinaryType)
{
  if (aBinaryType.EqualsLiteral("arraybuffer")) {
    mBinaryType = DC_BINARY_TYPE_ARRAYBUFFER;
  } else if (aBinaryType.EqualsLiteral("blob")) {
    mBinaryType = DC_BINARY_TYPE_BLOB;
  } else  {
    return NS_ERROR_INVALID_ARG;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDOMDataChannel::Close()
{
  mDataChannel->Close();
  return NS_OK;
}

// Almost a clone of nsWebSocketChannel::Send()
NS_IMETHODIMP
nsDOMDataChannel::Send(nsIVariant *aData, JSContext *aCx)
{
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "Not running on main thread");
  PRUint16 state = mDataChannel->GetReadyState();

  // In reality, the DataChannel protocol allows this, but we want it to
  // look like WebSockets
  if (state == nsIDOMDataChannel::CONNECTING) {
    return NS_ERROR_DOM_INVALID_STATE_ERR;
  }

  nsCString msgString;
  nsCOMPtr<nsIInputStream> msgStream;
  bool isBinary;
  PRUint32 msgLen;
  nsresult rv = GetSendParams(aData, msgString, msgStream, isBinary, msgLen, aCx);
  NS_ENSURE_SUCCESS(rv, rv);

  // Always increment outgoing buffer len, even if closed
  //mOutgoingBufferedAmount += msgLen;

  if (state == nsIDOMDataChannel::CLOSING ||
      state == nsIDOMDataChannel::CLOSED) {
    return NS_OK;
  }

  MOZ_ASSERT(state == nsIDOMDataChannel::OPEN,
             "Unknown state in nsWebSocket::Send");

  if (msgStream) {
    rv = mDataChannel->SendBinaryStream(msgStream, msgLen);
  } else {
    if (isBinary) {
      rv = mDataChannel->SendBinaryMsg(msgString);
    } else {
      rv = mDataChannel->SendMsg(msgString);
    }
  }
  NS_ENSURE_SUCCESS(rv, rv);

  //UpdateMustKeepAlive();

  return NS_OK;
}

// XXX Exact clone of nsWebSocketChannel::GetSendParams() - share!
nsresult
nsDOMDataChannel::GetSendParams(nsIVariant *aData, nsCString &aStringOut,
                                nsCOMPtr<nsIInputStream> &aStreamOut,
                                bool &aIsBinary, PRUint32 &aOutgoingLength,
                                JSContext *aCx)
{
  // Get type of data (arraybuffer, blob, or string)
  PRUint16 dataType;
  nsresult rv = aData->GetDataType(&dataType);
  NS_ENSURE_SUCCESS(rv, rv);

  if (dataType == nsIDataType::VTYPE_INTERFACE ||
      dataType == nsIDataType::VTYPE_INTERFACE_IS) {
    nsCOMPtr<nsISupports> supports;
    nsID *iid;
    rv = aData->GetAsInterface(&iid, getter_AddRefs(supports));
    NS_ENSURE_SUCCESS(rv, rv);

    nsMemory::Free(iid);

    // ArrayBuffer?
    jsval realVal;
    JSObject* obj;
    nsresult rv = aData->GetAsJSVal(&realVal);
    if (NS_SUCCEEDED(rv) && !JSVAL_IS_PRIMITIVE(realVal) &&
        (obj = JSVAL_TO_OBJECT(realVal)) &&
        (JS_IsArrayBufferObject(obj, aCx))) {
      PRInt32 len = JS_GetArrayBufferByteLength(obj, aCx);
      char* data = reinterpret_cast<char*>(JS_GetArrayBufferData(obj, aCx));

      aStringOut.Assign(data, len);
      aIsBinary = true;
      aOutgoingLength = len;
      return NS_OK;
    }

    // Blob?
    nsCOMPtr<nsIDOMBlob> blob = do_QueryInterface(supports);
    if (blob) {
      rv = blob->GetInternalStream(getter_AddRefs(aStreamOut));
      NS_ENSURE_SUCCESS(rv, rv);

      // GetSize() should not perform blocking I/O (unlike Available())
      PRUint64 blobLen;
      rv = blob->GetSize(&blobLen);
      NS_ENSURE_SUCCESS(rv, rv);
      if (blobLen > PR_UINT32_MAX) {
        return NS_ERROR_FILE_TOO_BIG;
      }
      aOutgoingLength = static_cast<PRUint32>(blobLen);

      aIsBinary = true;
      return NS_OK;
    }
  }

  // Text message: if not already a string, turn it into one.
  // TODO: bug 704444: Correctly coerce any JS type to string
  //
  PRUnichar* data = nsnull;
  PRUint32 len = 0;
  rv = aData->GetAsWStringWithSize(&len, &data);
  NS_ENSURE_SUCCESS(rv, rv);

  nsString text;
  text.Adopt(data, len);

  CopyUTF16toUTF8(text, aStringOut);

  aIsBinary = false;
  aOutgoingLength = aStringOut.Length();
  return NS_OK;
}

// Initial implementation: only stores to RAM, not file
// TODO: bug 704447: large file support
nsresult
nsDOMDataChannel::CreateResponseBlob(const nsACString& aData, JSContext *aCx,
                                     jsval &jsData)
{
  PRUint32 blobLen = aData.Length();
  void *blobData = PR_Malloc(blobLen);
  nsCOMPtr<nsIDOMBlob> blob;
  if (blobData) {
    memcpy(blobData, aData.BeginReading(), blobLen);
    blob = new nsDOMMemoryFile(blobData, blobLen, EmptyString());
  } else {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  JSObject* scope = JS_GetGlobalForScopeChain(aCx);
  return nsContentUtils::WrapNative(aCx, scope, blob, &jsData, nullptr, true);
}

nsresult
nsDOMDataChannel::DoOnMessageAvailable(const nsACString& aData,
                                       bool isBinary)
{
  nsresult rv;
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "Not running on main thread");

  LOG(("DoOnMessageAvailable%s\n",isBinary ? ((mBinaryType == DC_BINARY_TYPE_BLOB) ? " (blob)" : " (binary)" : "")));

  nsCOMPtr<nsIScriptGlobalObject> sgo = do_QueryInterface(GetOwner());
  NS_ENSURE_TRUE(sgo, NS_ERROR_FAILURE);

  nsIScriptContext* sc = sgo->GetContext();
  NS_ENSURE_TRUE(sc, NS_ERROR_FAILURE);

  JSContext* cx = sc->GetNativeContext();
  NS_ENSURE_TRUE(cx, NS_ERROR_FAILURE);

  JSAutoRequest ar(cx);
  jsval jsData;

  if (isBinary) {
    if (mBinaryType == DC_BINARY_TYPE_BLOB) {
      rv = CreateResponseBlob(aData, cx, jsData);
      NS_ENSURE_SUCCESS(rv, rv);
    } else if (mBinaryType == DC_BINARY_TYPE_ARRAYBUFFER) {
      JSObject *arrayBuf;
      rv = nsContentUtils::CreateArrayBuffer(cx, aData, &arrayBuf);
      NS_ENSURE_SUCCESS(rv, rv);
      jsData = OBJECT_TO_JSVAL(arrayBuf);
    } else {
      NS_RUNTIMEABORT("Unknown binary type!");
      return NS_ERROR_UNEXPECTED;
    }
  } else {
    NS_ConvertUTF8toUTF16 utf16data(aData);
    JSString* jsString;
    jsString = JS_NewUCStringCopyN(cx, utf16data.get(), utf16data.Length());
    NS_ENSURE_TRUE(jsString, NS_ERROR_FAILURE);

    jsData = STRING_TO_JSVAL(jsString);
  }

  nsCOMPtr<nsIDOMEvent> event;
  rv = NS_NewDOMMessageEvent(getter_AddRefs(event), nsnull, nsnull);
  NS_ENSURE_SUCCESS(rv,rv);

  nsCOMPtr<nsIDOMMessageEvent> messageEvent = do_QueryInterface(event);
  rv = messageEvent->InitMessageEvent(NS_LITERAL_STRING("message"),
                                      false, false,
                                      jsData, mUTF16Origin, EmptyString(),
                                      nsnull);
  NS_ENSURE_SUCCESS(rv,rv);
  event->SetTrusted(true);

  nsRefPtr<nsIDOMEventListener> listener;
  GetOnmessage(getter_AddRefs(listener));

  LOG(("%p(%p): %s - Dispatching for %p\n",this,(void*)mDataChannel,__FUNCTION__,(void*)listener.get()));
  rv = DispatchDOMEvent(nsnull, event, nsnull, nsnull);
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to dispatch the message event!!!");
  }
  return rv;
}

nsresult
nsDOMDataChannel::OnMessageAvailable(nsISupports* aContext,
                                     const nsACString& aMessage)
{
  MOZ_ASSERT(NS_IsMainThread());
  return DoOnMessageAvailable(aMessage, false);
}

nsresult
nsDOMDataChannel::OnBinaryMessageAvailable(nsISupports* aContext,
                                           const nsACString& aMessage)
{
  MOZ_ASSERT(NS_IsMainThread());
  return DoOnMessageAvailable(aMessage, true);
}

nsresult
nsDOMDataChannel::OnChannelConnected(nsISupports* aContext)
{
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIDOMEvent> event;
  nsresult rv = NS_NewDOMEvent(getter_AddRefs(event), nsnull, nsnull);
  NS_ENSURE_SUCCESS(rv,rv);

  rv = event->InitEvent(NS_LITERAL_STRING("open"), false, false);
  NS_ENSURE_SUCCESS(rv,rv);

  event->SetTrusted(true);

  LOG(("%p(%p): %s - Dispatching\n",this,(void*)mDataChannel,__FUNCTION__));

  return DispatchDOMEvent(nsnull, event, nsnull, nsnull);
}

nsresult
nsDOMDataChannel::OnChannelClosed(nsISupports* aContext)
{
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIDOMEvent> event;
  nsresult rv = NS_NewDOMEvent(getter_AddRefs(event), nsnull, nsnull);
  NS_ENSURE_SUCCESS(rv,rv);

  rv = event->InitEvent(NS_LITERAL_STRING("close"), false, false);
  NS_ENSURE_SUCCESS(rv,rv);

  event->SetTrusted(true);

  LOG(("%p(%p): %s - Dispatching\n",this,(void*)mDataChannel,__FUNCTION__));

  return DispatchDOMEvent(nsnull, event, nsnull, nsnull);
}

/* static */
nsresult
NS_NewDOMDataChannel(mozilla::DataChannel* dataChannel,
                     nsPIDOMWindow* aWindow,
                     nsIDOMDataChannel** domDataChannel)
{
  nsresult rv;

  nsRefPtr<nsDOMDataChannel> domdc = new nsDOMDataChannel(dataChannel);

  rv = domdc->Init(aWindow);
  NS_ENSURE_SUCCESS(rv,rv);

  return CallQueryInterface(domdc, domDataChannel);
}
