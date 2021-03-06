/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nsIMemoryReporter.h"
#include "nsCOMArray.h"
#include "mozilla/Mutex.h"
#include "mozilla/Attributes.h"
#include "nsString.h"

using mozilla::Mutex;

class nsMemoryReporter MOZ_FINAL : public nsIMemoryReporter
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMEMORYREPORTER

  nsMemoryReporter(nsACString& process,
                   nsACString& path, 
                   int32_t kind,
                   int32_t units,
                   int64_t amount,
                   nsACString& desc);

  ~nsMemoryReporter();

protected:
  nsCString mProcess;
  nsCString mPath;
  int32_t   mKind;
  int32_t   mUnits;
  int64_t   mAmount;
  nsCString mDesc;
};


class nsMemoryReporterManager : public nsIMemoryReporterManager
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIMEMORYREPORTERMANAGER

    nsMemoryReporterManager();
    virtual ~nsMemoryReporterManager();

private:
    nsresult DumpMemoryReportsToFileImpl(const nsAString& aIdentifier);

    nsCOMArray<nsIMemoryReporter>      mReporters;
    nsCOMArray<nsIMemoryMultiReporter> mMultiReporters;
    Mutex                              mMutex;
};

#define NS_MEMORY_REPORTER_MANAGER_CID \
{ 0xfb97e4f5, 0x32dd, 0x497a, \
{ 0xba, 0xa2, 0x7d, 0x1e, 0x55, 0x7, 0x99, 0x10 } }
