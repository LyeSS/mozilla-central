/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998-1999 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef AggregatePlaceholderTxn_h__
#define AggregatePlaceholderTxn_h__

#include "EditAggregateTxn.h"
#include "nsIAbsorbingTransaction.h"
#include "nsIDOMNode.h"
#include "nsCOMPtr.h"
#include "nsWeakPtr.h"
#include "nsWeakReference.h"

#define PLACEHOLDER_TXN_CID \
{/* {0CE9FB00-D9D1-11d2-86DE-000064657374} */ \
0x0CE9FB00, 0xD9D1, 0x11d2, \
{0x86, 0xde, 0x0, 0x0, 0x64, 0x65, 0x73, 0x74} }

class nsHTMLEditor;

/**
 * An aggregate transaction that knows how to absorb all subsequent
 * transactions with the same name.  This transaction does not "Do" anything.
 * But it absorbs other transactions via merge, and can undo/redo the
 * transactions it has absorbed.
 */
 
class PlaceholderTxn : public EditAggregateTxn, 
                       public nsIAbsorbingTransaction, 
                       public nsSupportsWeakReference
{
public:

  static const nsIID& GetCID() { static nsIID iid = PLACEHOLDER_TXN_CID; return iid; }

  NS_DECL_ISUPPORTS_INHERITED  
  
private:
  PlaceholderTxn();

public:

  virtual ~PlaceholderTxn();

// ------------ EditAggregateTxn -----------------------

  NS_IMETHOD Do(void);

  NS_IMETHOD Undo(void);

  NS_IMETHOD Merge(PRBool *aDidMerge, nsITransaction *aTransaction);

// ------------ nsIAbsorbingTransaction -----------------------

  NS_IMETHOD Init(nsWeakPtr aPresShellWeak, nsIAtom *aName, nsIDOMNode *aStartNode, PRInt32 aStartOffset);
  
  NS_IMETHOD GetTxnName(nsIAtom **aName);
  
  NS_IMETHOD GetStartNodeAndOffset(nsCOMPtr<nsIDOMNode> *aTxnStartNode, PRInt32 *aTxnStartOffset);

  NS_IMETHOD EndPlaceHolderBatch();

  NS_IMETHOD ForwardEndBatchTo(nsIAbsorbingTransaction *aForwardingAddress);

  friend class TransactionFactory;

  enum { kTransactionID = 11260 };

protected:

  /** the presentation shell, which we'll need to get the selection */
  nsWeakPtr mPresShellWeak;   // weak reference to the nsIPresShell
  PRBool    mAbsorb;
  nsCOMPtr<nsIDOMNode> mStartNode, mEndNode; // selection nodes at beginning and end of operation
  PRInt32   mStartOffset, mEndOffset;      // selection offsets at beginning and end of operation
  nsWeakPtr mForwarding;
};


#endif
