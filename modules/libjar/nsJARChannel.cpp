/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsJAR.h"
#include "nsJARChannel.h"
#include "nsJARProtocolHandler.h"
#include "nsMimeTypes.h"
#include "nsNetUtil.h"
#include "nsEscape.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsIViewSourceChannel.h"
#include "nsChannelProperties.h"

#include "nsIScriptSecurityManager.h"
#include "nsIPrincipal.h"
#include "nsIFileURL.h"

#include "mozilla/Preferences.h"

using namespace mozilla;

static NS_DEFINE_CID(kZipReaderCID, NS_ZIPREADER_CID);

// the entry for a directory will either be empty (in the case of the
// top-level directory) or will end with a slash
#define ENTRY_IS_DIRECTORY(_entry) \
  ((_entry).IsEmpty() || '/' == (_entry).Last())

//-----------------------------------------------------------------------------

#if defined(PR_LOGGING)
//
// set NSPR_LOG_MODULES=nsJarProtocol:5
//
static PRLogModuleInfo *gJarProtocolLog = nullptr;
#endif

#define LOG(args)     PR_LOG(gJarProtocolLog, PR_LOG_DEBUG, args)
#define LOG_ENABLED() PR_LOG_TEST(gJarProtocolLog, 4)

//-----------------------------------------------------------------------------
// nsJARInputThunk
//
// this class allows us to do some extra work on the stream transport thread.
//-----------------------------------------------------------------------------

class nsJARInputThunk : public nsIInputStream
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIINPUTSTREAM

    nsJARInputThunk(nsIZipReader *zipReader,
                    nsIURI* fullJarURI,
                    const nsACString &jarEntry,
                    nsIZipReaderCache *jarCache)
        : mJarCache(jarCache)
        , mJarReader(zipReader)
        , mJarEntry(jarEntry)
        , mContentLength(-1)
    {
        if (fullJarURI) {
#ifdef DEBUG
            nsresult rv =
#endif
                fullJarURI->GetAsciiSpec(mJarDirSpec);
            NS_ASSERTION(NS_SUCCEEDED(rv), "this shouldn't fail");
        }
    }

    virtual ~nsJARInputThunk()
    {
        if (!mJarCache && mJarReader)
            mJarReader->Close();
    }

    void GetJarReader(nsIZipReader **result)
    {
        NS_IF_ADDREF(*result = mJarReader);
    }

    int32_t GetContentLength()
    {
        return mContentLength;
    }

    nsresult EnsureJarStream();

private:

    nsCOMPtr<nsIZipReaderCache> mJarCache;
    nsCOMPtr<nsIZipReader>      mJarReader;
    nsCString                   mJarDirSpec;
    nsCOMPtr<nsIInputStream>    mJarStream;
    nsCString                   mJarEntry;
    int32_t                     mContentLength;
};

NS_IMPL_THREADSAFE_ISUPPORTS1(nsJARInputThunk, nsIInputStream)

nsresult
nsJARInputThunk::EnsureJarStream()
{
    if (mJarStream)
        return NS_OK;

    nsresult rv;
    if (ENTRY_IS_DIRECTORY(mJarEntry)) {
        // A directory stream also needs the Spec of the FullJarURI
        // because is included in the stream data itself.

        NS_ENSURE_STATE(!mJarDirSpec.IsEmpty());

        rv = mJarReader->GetInputStreamWithSpec(mJarDirSpec,
                                                mJarEntry,
                                                getter_AddRefs(mJarStream));
    }
    else {
        rv = mJarReader->GetInputStream(mJarEntry,
                                        getter_AddRefs(mJarStream));
    }
    if (NS_FAILED(rv)) {
        // convert to the proper result if the entry wasn't found
        // so that error pages work
        if (rv == NS_ERROR_FILE_TARGET_DOES_NOT_EXIST)
            rv = NS_ERROR_FILE_NOT_FOUND;
        return rv;
    }

    // ask the JarStream for the content length
    uint64_t avail;
    rv = mJarStream->Available((uint64_t *) &avail);
    if (NS_FAILED(rv)) return rv;

    mContentLength = avail < INT32_MAX ? (int32_t) avail : -1;

    return NS_OK;
}

NS_IMETHODIMP
nsJARInputThunk::Close()
{
    if (mJarStream)
        return mJarStream->Close();

    return NS_OK;
}

NS_IMETHODIMP
nsJARInputThunk::Available(uint64_t *avail)
{
    nsresult rv = EnsureJarStream();
    if (NS_FAILED(rv)) return rv;

    return mJarStream->Available(avail);
}

NS_IMETHODIMP
nsJARInputThunk::Read(char *buf, uint32_t count, uint32_t *countRead)
{
    nsresult rv = EnsureJarStream();
    if (NS_FAILED(rv)) return rv;

    return mJarStream->Read(buf, count, countRead);
}

NS_IMETHODIMP
nsJARInputThunk::ReadSegments(nsWriteSegmentFun writer, void *closure,
                              uint32_t count, uint32_t *countRead)
{
    // stream transport does only calls Read()
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsJARInputThunk::IsNonBlocking(bool *nonBlocking)
{
    *nonBlocking = false;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsJARChannel
//-----------------------------------------------------------------------------


nsJARChannel::nsJARChannel()
    : mOpened(false)
    , mAppURI(nullptr)
    , mContentLength(-1)
    , mLoadFlags(LOAD_NORMAL)
    , mStatus(NS_OK)
    , mIsPending(false)
    , mIsUnsafe(true)
    , mJarInput(nullptr)
{
#if defined(PR_LOGGING)
    if (!gJarProtocolLog)
        gJarProtocolLog = PR_NewLogModule("nsJarProtocol");
#endif

    // hold an owning reference to the jar handler
    NS_ADDREF(gJarHandler);
}

nsJARChannel::~nsJARChannel()
{
    // with the exception of certain error cases mJarInput will already be null.
    NS_IF_RELEASE(mJarInput);

    // release owning reference to the jar handler
    nsJARProtocolHandler *handler = gJarHandler;
    NS_RELEASE(handler); // NULL parameter
}

NS_IMPL_ISUPPORTS_INHERITED6(nsJARChannel,
                             nsHashPropertyBag,
                             nsIRequest,
                             nsIChannel,
                             nsIStreamListener,
                             nsIRequestObserver,
                             nsIDownloadObserver,
                             nsIJARChannel)

nsresult 
nsJARChannel::Init(nsIURI *uri)
{
    nsresult rv;
    rv = nsHashPropertyBag::Init();
    if (NS_FAILED(rv))
        return rv;

    mJarURI = do_QueryInterface(uri, &rv);
    if (NS_FAILED(rv))
        return rv;

    mOriginalURI = mJarURI;

    // Prevent loading jar:javascript URIs (see bug 290982).
    nsCOMPtr<nsIURI> innerURI;
    rv = mJarURI->GetJARFile(getter_AddRefs(innerURI));
    if (NS_FAILED(rv))
        return rv;
    bool isJS;
    rv = innerURI->SchemeIs("javascript", &isJS);
    if (NS_FAILED(rv))
        return rv;
    if (isJS) {
        NS_WARNING("blocking jar:javascript:");
        return NS_ERROR_INVALID_ARG;
    }

#if defined(PR_LOGGING)
    mJarURI->GetSpec(mSpec);
#endif
    return rv;
}

nsresult
nsJARChannel::CreateJarInput(nsIZipReaderCache *jarCache)
{
    // important to pass a clone of the file since the nsIFile impl is not
    // necessarily MT-safe
    nsCOMPtr<nsIFile> clonedFile;
    nsresult rv = mJarFile->Clone(getter_AddRefs(clonedFile));
    if (NS_FAILED(rv))
        return rv;

    nsCOMPtr<nsIZipReader> reader;
    if (jarCache) {
        if (mInnerJarEntry.IsEmpty())
            rv = jarCache->GetZip(mJarFile, getter_AddRefs(reader));
        else 
            rv = jarCache->GetInnerZip(mJarFile, mInnerJarEntry,
                                       getter_AddRefs(reader));
    } else {
        // create an uncached jar reader
        nsCOMPtr<nsIZipReader> outerReader = do_CreateInstance(kZipReaderCID, &rv);
        if (NS_FAILED(rv))
            return rv;

        rv = outerReader->Open(mJarFile);
        if (NS_FAILED(rv))
            return rv;

        if (mInnerJarEntry.IsEmpty())
            reader = outerReader;
        else {
            reader = do_CreateInstance(kZipReaderCID, &rv);
            if (NS_FAILED(rv))
                return rv;

            rv = reader->OpenInner(outerReader, mInnerJarEntry);
        }
    }
    if (NS_FAILED(rv))
        return rv;

    mJarInput = new nsJARInputThunk(reader, mJarURI, mJarEntry, jarCache);
    if (!mJarInput)
        return NS_ERROR_OUT_OF_MEMORY;
    NS_ADDREF(mJarInput);
    return NS_OK;
}

nsresult
nsJARChannel::EnsureJarInput(bool blocking)
{
    LOG(("nsJARChannel::EnsureJarInput [this=%x %s]\n", this, mSpec.get()));

    nsresult rv;
    nsCOMPtr<nsIURI> uri;

    rv = mJarURI->GetJARFile(getter_AddRefs(mJarBaseURI));
    if (NS_FAILED(rv)) return rv;

    rv = mJarURI->GetJAREntry(mJarEntry);
    if (NS_FAILED(rv)) return rv;

    // The name of the JAR entry must not contain URL-escaped characters:
    // we're moving from URL domain to a filename domain here. nsStandardURL
    // does basic escaping by default, which breaks reading zipped files which
    // have e.g. spaces in their filenames.
    NS_UnescapeURL(mJarEntry);

    // try to get a nsIFile directly from the url, which will often succeed.
    {
        nsCOMPtr<nsIFileURL> fileURL = do_QueryInterface(mJarBaseURI);
        if (fileURL)
            fileURL->GetFile(getter_AddRefs(mJarFile));
    }
    // try to handle a nested jar
    if (!mJarFile) {
        nsCOMPtr<nsIJARURI> jarURI = do_QueryInterface(mJarBaseURI);
        if (jarURI) {
            nsCOMPtr<nsIFileURL> fileURL;
            nsCOMPtr<nsIURI> innerJarURI;
            rv = jarURI->GetJARFile(getter_AddRefs(innerJarURI));
            if (NS_SUCCEEDED(rv))
                fileURL = do_QueryInterface(innerJarURI);
            if (fileURL) {
                fileURL->GetFile(getter_AddRefs(mJarFile));
                jarURI->GetJAREntry(mInnerJarEntry);
            }
        }
    }

    if (mJarFile) {
        mIsUnsafe = false;

        // NOTE: we do not need to deal with mSecurityInfo here,
        // because we're loading from a local file
        rv = CreateJarInput(gJarHandler->JarCache());
    }
    else if (blocking) {
        NS_NOTREACHED("need sync downloader");
        rv = NS_ERROR_NOT_IMPLEMENTED;
    }
    else {
        // kick off an async download of the base URI...
        rv = NS_NewDownloader(getter_AddRefs(mDownloader), this);
        if (NS_SUCCEEDED(rv))
            rv = NS_OpenURI(mDownloader, nullptr, mJarBaseURI, nullptr,
                            mLoadGroup, mCallbacks,
                            mLoadFlags & ~(LOAD_DOCUMENT_URI | LOAD_CALL_CONTENT_SNIFFERS));
    }
    return rv;

}

//-----------------------------------------------------------------------------
// nsIRequest
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsJARChannel::GetName(nsACString &result)
{
    return mJarURI->GetSpec(result);
}

NS_IMETHODIMP
nsJARChannel::IsPending(bool *result)
{
    *result = mIsPending;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetStatus(nsresult *status)
{
    if (mPump && NS_SUCCEEDED(mStatus))
        mPump->GetStatus(status);
    else
        *status = mStatus;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::Cancel(nsresult status)
{
    mStatus = status;
    if (mPump)
        return mPump->Cancel(status);

    NS_ASSERTION(!mIsPending, "need to implement cancel when downloading");
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::Suspend()
{
    if (mPump)
        return mPump->Suspend();

    NS_ASSERTION(!mIsPending, "need to implement suspend when downloading");
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::Resume()
{
    if (mPump)
        return mPump->Resume();

    NS_ASSERTION(!mIsPending, "need to implement resume when downloading");
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetLoadFlags(nsLoadFlags *aLoadFlags)
{
    *aLoadFlags = mLoadFlags;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetLoadFlags(nsLoadFlags aLoadFlags)
{
    mLoadFlags = aLoadFlags;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetLoadGroup(nsILoadGroup **aLoadGroup)
{
    NS_IF_ADDREF(*aLoadGroup = mLoadGroup);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetLoadGroup(nsILoadGroup *aLoadGroup)
{
    mLoadGroup = aLoadGroup;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsIChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsJARChannel::GetOriginalURI(nsIURI **aURI)
{
    *aURI = mOriginalURI;
    NS_ADDREF(*aURI);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetOriginalURI(nsIURI *aURI)
{
    NS_ENSURE_ARG_POINTER(aURI);
    mOriginalURI = aURI;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetURI(nsIURI **aURI)
{
    if (mAppURI) {
        NS_IF_ADDREF(*aURI = mAppURI);
    } else {
        NS_IF_ADDREF(*aURI = mJarURI);
    }

    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetOwner(nsISupports **aOwner)
{
    // JAR signatures are not processed to avoid main-thread network I/O (bug 726125)
    *aOwner = mOwner;
    NS_IF_ADDREF(*aOwner);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetOwner(nsISupports *aOwner)
{
    mOwner = aOwner;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetNotificationCallbacks(nsIInterfaceRequestor **aCallbacks)
{
    NS_IF_ADDREF(*aCallbacks = mCallbacks);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetNotificationCallbacks(nsIInterfaceRequestor *aCallbacks)
{
    mCallbacks = aCallbacks;
    return NS_OK;
}

NS_IMETHODIMP 
nsJARChannel::GetSecurityInfo(nsISupports **aSecurityInfo)
{
    NS_PRECONDITION(aSecurityInfo, "Null out param");
    NS_IF_ADDREF(*aSecurityInfo = mSecurityInfo);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetContentType(nsACString &result)
{
    // If the Jar file has not been open yet,
    // We return application/x-unknown-content-type
    if (!mOpened) {
      result.Assign(UNKNOWN_CONTENT_TYPE);
      return NS_OK;
    }

    if (mContentType.IsEmpty()) {

        //
        // generate content type and set it
        //
        const char *ext = nullptr, *fileName = mJarEntry.get();
        int32_t len = mJarEntry.Length();

        // check if we're displaying a directory
        // mJarEntry will be empty if we're trying to display
        // the topmost directory in a zip, e.g. jar:foo.zip!/
        if (ENTRY_IS_DIRECTORY(mJarEntry)) {
            mContentType.AssignLiteral(APPLICATION_HTTP_INDEX_FORMAT);
        }
        else {
            // not a directory, take a guess by its extension
            for (int32_t i = len-1; i >= 0; i--) {
                if (fileName[i] == '.') {
                    ext = &fileName[i + 1];
                    break;
                }
            }
            if (ext) {
                nsIMIMEService *mimeServ = gJarHandler->MimeService();
                if (mimeServ)
                    mimeServ->GetTypeFromExtension(nsDependentCString(ext), mContentType);
            }
            if (mContentType.IsEmpty())
                mContentType.AssignLiteral(UNKNOWN_CONTENT_TYPE);
        }
    }
    result = mContentType;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetContentType(const nsACString &aContentType)
{
    // If someone gives us a type hint we should just use that type instead of
    // doing our guessing.  So we don't care when this is being called.

    // mContentCharset is unchanged if not parsed
    NS_ParseContentType(aContentType, mContentType, mContentCharset);
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetContentCharset(nsACString &aContentCharset)
{
    // If someone gives us a charset hint we should just use that charset.
    // So we don't care when this is being called.
    aContentCharset = mContentCharset;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetContentCharset(const nsACString &aContentCharset)
{
    mContentCharset = aContentCharset;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetContentDisposition(uint32_t *aContentDisposition)
{
    if (mContentDispositionHeader.IsEmpty())
        return NS_ERROR_NOT_AVAILABLE;

    *aContentDisposition = mContentDisposition;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetContentDisposition(uint32_t aContentDisposition)
{
    return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
nsJARChannel::GetContentDispositionFilename(nsAString &aContentDispositionFilename)
{
    return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
nsJARChannel::SetContentDispositionFilename(const nsAString &aContentDispositionFilename)
{
    return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
nsJARChannel::GetContentDispositionHeader(nsACString &aContentDispositionHeader)
{
    if (mContentDispositionHeader.IsEmpty())
        return NS_ERROR_NOT_AVAILABLE;

    aContentDispositionHeader = mContentDispositionHeader;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::GetContentLength(int32_t *result)
{
    // if content length is unknown, query mJarInput...
    if (mContentLength < 0 && mJarInput)
        mContentLength = mJarInput->GetContentLength();

    *result = mContentLength;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetContentLength(int32_t aContentLength)
{
    // XXX does this really make any sense at all?
    mContentLength = aContentLength;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::Open(nsIInputStream **stream)
{
    LOG(("nsJARChannel::Open [this=%x]\n", this));

    NS_ENSURE_TRUE(!mJarInput, NS_ERROR_IN_PROGRESS);
    NS_ENSURE_TRUE(!mIsPending, NS_ERROR_IN_PROGRESS);

    mJarFile = nullptr;
    mIsUnsafe = true;

    nsresult rv = EnsureJarInput(true);
    if (NS_FAILED(rv)) return rv;

    if (!mJarInput)
        return NS_ERROR_UNEXPECTED;

    // force load the jar file now so GetContentLength will return a
    // meaningful value once we return.
    rv = mJarInput->EnsureJarStream();
    if (NS_FAILED(rv)) return rv;

    NS_ADDREF(*stream = mJarInput);

    mOpened = true;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::AsyncOpen(nsIStreamListener *listener, nsISupports *ctx)
{
    LOG(("nsJARChannel::AsyncOpen [this=%x]\n", this));

    NS_ENSURE_ARG_POINTER(listener);
    NS_ENSURE_TRUE(!mIsPending, NS_ERROR_IN_PROGRESS);

    mJarFile = nullptr;
    mIsUnsafe = true;

    // Initialize mProgressSink
    NS_QueryNotificationCallbacks(mCallbacks, mLoadGroup, mProgressSink);

    nsresult rv = EnsureJarInput(false);
    if (NS_FAILED(rv)) return rv;

    // These variables must only be set if we're going to trigger an
    // OnStartRequest, either from AsyncRead or OnDownloadComplete.
    mListener = listener;
    mListenerContext = ctx;
    mIsPending = true;
    if (mJarInput) {
        // create input stream pump and call AsyncRead as a block
        rv = NS_NewInputStreamPump(getter_AddRefs(mPump), mJarInput);
        if (NS_SUCCEEDED(rv))
            rv = mPump->AsyncRead(this, nullptr);

        // If we failed to create the pump or initiate the AsyncRead,
        // then we need to clear these variables.
        if (NS_FAILED(rv)) {
            mIsPending = false;
            mListenerContext = nullptr;
            mListener = nullptr;
            return rv;
        }
    }

    if (mLoadGroup)
        mLoadGroup->AddRequest(this, nullptr);

    mOpened = true;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsIJARChannel
//-----------------------------------------------------------------------------
NS_IMETHODIMP
nsJARChannel::GetIsUnsafe(bool *isUnsafe)
{
    *isUnsafe = mIsUnsafe;
    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::SetAppURI(nsIURI *aURI) {
    NS_ENSURE_ARG_POINTER(aURI);

    nsAutoCString scheme;
    aURI->GetScheme(scheme);
    if (!scheme.EqualsLiteral("app")) {
        return NS_ERROR_INVALID_ARG;
    }

    mAppURI = aURI;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsIDownloadObserver
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsJARChannel::OnDownloadComplete(nsIDownloader *downloader,
                                 nsIRequest    *request,
                                 nsISupports   *context,
                                 nsresult       status,
                                 nsIFile       *file)
{
    nsresult rv;

    nsCOMPtr<nsIChannel> channel(do_QueryInterface(request));
    if (channel) {
        uint32_t loadFlags;
        channel->GetLoadFlags(&loadFlags);
        if (loadFlags & LOAD_REPLACE) {
            mLoadFlags |= LOAD_REPLACE;

            if (!mOriginalURI) {
                SetOriginalURI(mJarURI);
            }

            nsCOMPtr<nsIURI> innerURI;
            rv = channel->GetURI(getter_AddRefs(innerURI));
            if (NS_SUCCEEDED(rv)) {
                nsCOMPtr<nsIJARURI> newURI;
                rv = mJarURI->CloneWithJARFile(innerURI,
                                               getter_AddRefs(newURI));
                if (NS_SUCCEEDED(rv)) {
                    mJarURI = newURI;
                }
            }
            if (NS_SUCCEEDED(status)) {
                status = rv;
            }
        }
    }

    if (NS_SUCCEEDED(status) && channel) {
        // Grab the security info from our base channel
        channel->GetSecurityInfo(getter_AddRefs(mSecurityInfo));

        nsCOMPtr<nsIHttpChannel> httpChannel(do_QueryInterface(channel));
        if (httpChannel) {
            // We only want to run scripts if the server really intended to
            // send us a JAR file.  Check the server-supplied content type for
            // a JAR type.
            nsAutoCString header;
            httpChannel->GetResponseHeader(NS_LITERAL_CSTRING("Content-Type"),
                                           header);
            nsAutoCString contentType;
            nsAutoCString charset;
            NS_ParseContentType(header, contentType, charset);
            nsAutoCString channelContentType;
            channel->GetContentType(channelContentType);
            mIsUnsafe = !(contentType.Equals(channelContentType) &&
                          (contentType.EqualsLiteral("application/java-archive") ||
                           contentType.EqualsLiteral("application/x-jar")));
        } else {
            nsCOMPtr<nsIJARChannel> innerJARChannel(do_QueryInterface(channel));
            if (innerJARChannel) {
                bool unsafe;
                innerJARChannel->GetIsUnsafe(&unsafe);
                mIsUnsafe = unsafe;
            }
        }

        channel->GetContentDispositionHeader(mContentDispositionHeader);
        mContentDisposition = NS_GetContentDispositionFromHeader(mContentDispositionHeader, this);
    }

    if (NS_SUCCEEDED(status) && mIsUnsafe &&
        !Preferences::GetBool("network.jar.open-unsafe-types", false)) {
        status = NS_ERROR_UNSAFE_CONTENT_TYPE;
    }

    if (NS_SUCCEEDED(status)) {
        // Refuse to unpack view-source: jars even if open-unsafe-types is set.
        nsCOMPtr<nsIViewSourceChannel> viewSource = do_QueryInterface(channel);
        if (viewSource) {
            status = NS_ERROR_UNSAFE_CONTENT_TYPE;
        }
    }

    if (NS_SUCCEEDED(status)) {
        mJarFile = file;
    
        rv = CreateJarInput(nullptr);
        if (NS_SUCCEEDED(rv)) {
            // create input stream pump
            rv = NS_NewInputStreamPump(getter_AddRefs(mPump), mJarInput);
            if (NS_SUCCEEDED(rv))
                rv = mPump->AsyncRead(this, nullptr);
        }
        status = rv;
    }

    if (NS_FAILED(status)) {
        mStatus = status;
        OnStartRequest(nullptr, nullptr);
        OnStopRequest(nullptr, nullptr, status);
    }

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsIStreamListener
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsJARChannel::OnStartRequest(nsIRequest *req, nsISupports *ctx)
{
    LOG(("nsJARChannel::OnStartRequest [this=%x %s]\n", this, mSpec.get()));

    return mListener->OnStartRequest(this, mListenerContext);
}

NS_IMETHODIMP
nsJARChannel::OnStopRequest(nsIRequest *req, nsISupports *ctx, nsresult status)
{
    LOG(("nsJARChannel::OnStopRequest [this=%x %s status=%x]\n",
        this, mSpec.get(), status));

    if (NS_SUCCEEDED(mStatus))
        mStatus = status;

    if (mListener) {
        mListener->OnStopRequest(this, mListenerContext, status);
        mListener = 0;
        mListenerContext = 0;
    }

    if (mLoadGroup)
        mLoadGroup->RemoveRequest(this, nullptr, status);

    mPump = 0;
    NS_IF_RELEASE(mJarInput);
    mIsPending = false;
    mDownloader = 0; // this may delete the underlying jar file

    // Drop notification callbacks to prevent cycles.
    mCallbacks = 0;
    mProgressSink = 0;

    return NS_OK;
}

NS_IMETHODIMP
nsJARChannel::OnDataAvailable(nsIRequest *req, nsISupports *ctx,
                               nsIInputStream *stream,
                               uint64_t offset, uint32_t count)
{
#if defined(PR_LOGGING)
    LOG(("nsJARChannel::OnDataAvailable [this=%x %s]\n", this, mSpec.get()));
#endif

    nsresult rv;

    rv = mListener->OnDataAvailable(this, mListenerContext, stream, offset, count);

    // simply report progress here instead of hooking ourselves up as a
    // nsITransportEventSink implementation.
    // XXX do the 64-bit stuff for real
    if (mProgressSink && NS_SUCCEEDED(rv) && !(mLoadFlags & LOAD_BACKGROUND))
        mProgressSink->OnProgress(this, nullptr, offset + count,
                                  uint64_t(mContentLength));

    return rv; // let the pump cancel on failure
}
