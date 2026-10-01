/*
 *  Copyright (c) 2019, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file includes definitions for handling indirect transmission.
 */

#include "indirect_sender.hpp"

#include "instance/instance.hpp"

namespace ot {

RegisterLogModule("IndirectSender");

#if OPENTHREAD_FTD || OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE

const Mac::Address &IndirectSender::NeighborInfo::GetMacAddress(Mac::Address &aMacAddress) const
{
    if (mUseShortAddress)
    {
        aMacAddress.SetShort(static_cast<const CslNeighbor *>(this)->GetRloc16());
    }
    else
    {
        aMacAddress.SetExtended(static_cast<const CslNeighbor *>(this)->GetExtAddress());
    }

    return aMacAddress;
}

IndirectSender::IndirectSender(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mEnabled(false)
#if OPENTHREAD_FTD
    , mSourceMatchController(aInstance)
    , mDataPollHandler(aInstance)
#endif
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    , mCslTxScheduler(aInstance)
#endif
{
}

void IndirectSender::Stop(void)
{
    VerifyOrExit(mEnabled);

#if OPENTHREAD_FTD
    for (Child &child : Get<ChildTable>().Iterate(Child::kInStateAnyExceptInvalid))
    {
        child.SetIndirectMessage(nullptr);
        mSourceMatchController.ResetMessageCount(child);
    }

    mDataPollHandler.Clear();
#endif

#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Clear();
#endif

exit:
    mEnabled = false;
}

bool IndirectSender::AcceptAnyMessage(const Message &aMessage)
{
    OT_UNUSED_VARIABLE(aMessage);

    return true;
}

#if OPENTHREAD_FTD

void IndirectSender::AddMessageForSleepyChild(Message &aMessage, Child &aChild)
{
    uint16_t childIndex;

    OT_ASSERT(!aChild.IsRxOnWhenIdle());

    childIndex = Get<ChildTable>().GetChildIndex(aChild);
    VerifyOrExit(!aMessage.GetIndirectTxChildMask().Has(childIndex));

    aMessage.GetIndirectTxChildMask().Add(childIndex);
    mSourceMatchController.IncrementMessageCount(aChild);

    if ((aMessage.GetType() != Message::kTypeSupervision) && (aChild.GetIndirectMessageCount() > 1))
    {
        Message *supervisionMessage = FindQueuedMessageForSleepyChild(aChild, AcceptSupervisionMessage);

        if (supervisionMessage != nullptr)
        {
            IgnoreError(RemoveMessageFromSleepyChild(*supervisionMessage, aChild));
            Get<MeshForwarder>().RemoveMessageIfNoPendingTx(*supervisionMessage);
        }
    }

    RequestMessageUpdate(aChild);

exit:
    return;
}

Error IndirectSender::RemoveMessageFromSleepyChild(Message &aMessage, Child &aChild)
{
    Error    error      = kErrorNone;
    uint16_t childIndex = Get<ChildTable>().GetChildIndex(aChild);

    VerifyOrExit(aMessage.GetIndirectTxChildMask().Has(childIndex), error = kErrorNotFound);

    aMessage.GetIndirectTxChildMask().Remove(childIndex);
    mSourceMatchController.DecrementMessageCount(aChild);

    RequestMessageUpdate(aChild);

exit:
    return error;
}

void IndirectSender::ClearAllMessagesForSleepyChild(Child &aChild)
{
    VerifyOrExit(aChild.GetIndirectMessageCount() > 0);

    for (Message &message : Get<MeshForwarder>().mSendQueue)
    {
        message.GetIndirectTxChildMask().Remove(Get<ChildTable>().GetChildIndex(aChild));

        Get<MeshForwarder>().RemoveMessageIfNoPendingTx(message);
    }

    aChild.SetIndirectMessage(nullptr);
    mSourceMatchController.ResetMessageCount(aChild);

    mDataPollHandler.RequestFrameChange(DataPollHandler::kPurgeFrame, aChild);
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Update();
#endif

exit:
    return;
}

const Message *IndirectSender::FindQueuedMessageForSleepyChild(const Child &aChild, MessageChecker aChecker) const
{
    const Message *match      = nullptr;
    uint16_t       childIndex = Get<ChildTable>().GetChildIndex(aChild);

    for (const Message &message : Get<MeshForwarder>().mSendQueue)
    {
        if (message.GetIndirectTxChildMask().Has(childIndex) && aChecker(message))
        {
            match = &message;
            break;
        }
    }

    return match;
}

void IndirectSender::SetChildUseShortAddress(Child &aChild, bool aUseShortAddress)
{
    VerifyOrExit(aChild.IsIndirectSourceMatchShort() != aUseShortAddress);

    mSourceMatchController.SetSrcMatchAsShort(aChild, aUseShortAddress);

exit:
    return;
}

void IndirectSender::HandleChildModeChange(Child &aChild, Mle::DeviceMode aOldMode)
{
    if (!aChild.IsRxOnWhenIdle() && (aChild.IsStateValid()))
    {
        SetChildUseShortAddress(aChild, true);
    }

    // On sleepy to non-sleepy mode change, convert indirect messages in
    // the send queue destined to the child to direct.

    if (!aOldMode.IsRxOnWhenIdle() && aChild.IsRxOnWhenIdle() && (aChild.GetIndirectMessageCount() > 0))
    {
        uint16_t childIndex = Get<ChildTable>().GetChildIndex(aChild);

        for (Message &message : Get<MeshForwarder>().mSendQueue)
        {
            if (message.GetIndirectTxChildMask().Has(childIndex))
            {
                message.GetIndirectTxChildMask().Remove(childIndex);
                message.SetDirectTransmission();
                message.SetTimestampToNow();
            }
        }

        aChild.SetIndirectMessage(nullptr);
        mSourceMatchController.ResetMessageCount(aChild);

        mDataPollHandler.RequestFrameChange(DataPollHandler::kPurgeFrame, aChild);
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
        mCslTxScheduler.Update();
#endif
    }

    // Since the queuing delays for direct transmissions are expected to
    // be relatively small especially when compared to indirect, for a
    // non-sleepy to sleepy mode change, we allow any direct message
    // (for the child) already in the send queue to remain as is. This
    // is equivalent to dropping the already queued messages in this
    // case.
}

void IndirectSender::RequestMessageUpdate(Child &aChild)
{
    Message *curMessage = aChild.GetIndirectMessage();
    Message *newMessage;

    // Purge the frame if the current message is no longer destined
    // for the child. This check needs to be done first to cover the
    // case where we have a pending "replace frame" request and while
    // waiting for the callback, the current message is removed.

    if ((curMessage != nullptr) && !curMessage->GetIndirectTxChildMask().Has(Get<ChildTable>().GetChildIndex(aChild)))
    {
        // Set the indirect message for this child to `nullptr` to ensure
        // it is not processed on `HandleSentFrameToChild()` callback.

        aChild.SetIndirectMessage(nullptr);

        // Request a "frame purge" using `RequestFrameChange()` and
        // wait for `HandleFrameChangeDone()` callback for completion
        // of the request. Note that the callback may be directly
        // called from the `RequestFrameChange()` itself when the
        // request can be handled immediately.

        aChild.SetWaitingForMessageUpdate(true);
        mDataPollHandler.RequestFrameChange(DataPollHandler::kPurgeFrame, aChild);
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
        mCslTxScheduler.Update();
#endif

        ExitNow();
    }

    VerifyOrExit(!aChild.IsWaitingForMessageUpdate());

    newMessage = FindQueuedMessageForSleepyChild(aChild, AcceptAnyMessage);

    VerifyOrExit(curMessage != newMessage);

    if (curMessage == nullptr)
    {
        // Current message is `nullptr`, but new message is not.
        // We have a new indirect message.

        UpdateIndirectMessage(aChild);
        ExitNow();
    }

    // Current message and new message differ and are both
    // non-`nullptr`. We need to request the frame to be replaced.
    // The current indirect message can be replaced only if it is
    // the first fragment. If a next fragment frame for message is
    // already prepared, we wait for the entire message to be
    // delivered.

    VerifyOrExit(aChild.GetIndirectFragmentOffset() == 0);

    aChild.SetWaitingForMessageUpdate(true);
    mDataPollHandler.RequestFrameChange(DataPollHandler::kReplaceFrame, aChild);
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Update();
#endif

exit:
    return;
}

void IndirectSender::HandleFrameChangeDone(Child &aChild)
{
    VerifyOrExit(aChild.IsWaitingForMessageUpdate());
    UpdateIndirectMessage(aChild);

exit:
    return;
}

void IndirectSender::UpdateIndirectMessage(Child &aChild)
{
    Message *message = FindQueuedMessageForSleepyChild(aChild, AcceptAnyMessage);

    aChild.SetWaitingForMessageUpdate(false);
    aChild.SetIndirectMessage(message);
    aChild.SetIndirectFragmentOffset(0);
    aChild.SetIndirectTxSuccess(true);

#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Update();
#endif

    if (message != nullptr)
    {
        Mac::Address childAddress;

        aChild.GetMacAddress(childAddress);
        Get<MeshForwarder>().LogMessage(MeshForwarder::kMessagePrepareIndirect, *message, kErrorNone, &childAddress);
    }
}

Error IndirectSender::PrepareFrameForChild(Mac::TxFrame &aFrame, FrameContext &aContext, Child &aChild)
{
    Error          error = kErrorNone;
    Message       *message;
    Ip6::Header    ip6Header;
    Mac::Addresses macAddrs;
    uint16_t       directTxOffset;

    VerifyOrExit(mEnabled, error = kErrorAbort);

    aChild.GetMacAddress(macAddrs.mDestination);

    message = aChild.GetIndirectMessage();

    if ((message == nullptr) || (message->GetType() == Message::kTypeSupervision))
    {
        Get<MessageFramer>().PrepareEmptyFrame(aFrame, macAddrs.mDestination, /* aAckRequest */ true);
        aContext.mMessageNextOffset = (message == nullptr) ? 0 : message->GetLength();

        ExitNow();
    }

    VerifyOrExit(message->GetType() == Message::kTypeIp6);

    // Determine the MAC source and destination addresses.

    IgnoreError(message->Read(0, ip6Header));

    Get<MessageFramer>().DetermineMacSourceAddress(ip6Header.GetSource(), macAddrs);

    if (ip6Header.GetDestination().IsLinkLocalUnicast())
    {
        macAddrs.mDestination.SetExtendedFromIid(ip6Header.GetDestination().GetIid());
    }

    // Prepare the data frame from previous child's indirect offset.

    directTxOffset = message->GetOffset();
    message->SetOffset(aChild.GetIndirectFragmentOffset());

    aContext.mMessageNextOffset = Get<MessageFramer>().PrepareFrame(aFrame, *message, macAddrs);

    message->SetOffset(directTxOffset);

    // Set `FramePending` if there are more queued messages (excluding
    // the current one being sent out) for the child (note `> 1` check).
    // The case where the current message itself requires fragmentation
    // is already checked and handled in the above `PrepareFrame` call.

    if (aChild.GetIndirectMessageCount() > 1)
    {
        aFrame.SetFramePending(true);
    }

exit:
    return error;
}

void IndirectSender::HandleSentFrameToChild(const Mac::TxFrame &aFrame,
                                            const FrameContext &aContext,
                                            Error               aError,
                                            Child              &aChild)
{
    Message *message    = aChild.GetIndirectMessage();
    uint16_t nextOffset = aContext.mMessageNextOffset;

    VerifyOrExit(mEnabled);

    if (aError == kErrorNone)
    {
        Get<ChildSupervisor>().UpdateOnSend(aChild);
    }

    // A zero `nextOffset` indicates that the sent frame is an empty
    // frame generated by `PrepareFrameForChild()` when there was no
    // indirect message in the send queue for the child. This can happen
    // in the (not common) case where the radio platform does not
    // support the "source address match" feature and always includes
    // "frame pending" flag in acks to data poll frames. In such a case,
    // `IndirectSender` prepares and sends an empty frame to the child
    // after it sends a data poll. Here in `HandleSentFrameToChild()` we
    // exit quickly if we detect the "send done" is for the empty frame
    // to ensure we do not update any newly added indirect message after
    // preparing the empty frame.

    VerifyOrExit(nextOffset != 0);

    switch (aError)
    {
    case kErrorNone:
        break;

    case kErrorNoAck:
    case kErrorChannelAccessFailure:
    case kErrorAbort:

        aChild.SetIndirectTxSuccess(false);

#if OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE
        // We set the nextOffset to end of message, since there is no need to
        // send any remaining fragments in the message to the child, if all tx
        // attempts of current frame already failed.

        if (message != nullptr)
        {
            nextOffset = message->GetLength();
        }
#endif
        break;

    default:
        OT_ASSERT(false);
    }

    if ((message != nullptr) && (nextOffset < message->GetLength()))
    {
        aChild.SetIndirectFragmentOffset(nextOffset);
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
        mCslTxScheduler.Update();
#endif
        ExitNow();
    }

    if (message != nullptr)
    {
        // The indirect tx of this message to the child is done.

        Error        txError    = aError;
        uint16_t     childIndex = Get<ChildTable>().GetChildIndex(aChild);
        Mac::Address macDest;

        aChild.SetIndirectMessage(nullptr);
        aChild.GetLinkInfo().AddMessageTxStatus(aChild.GetIndirectTxSuccess());

        // Enable short source address matching after the first indirect
        // message transmission attempt to the child. We intentionally do
        // not check for successful tx here to address the scenario where
        // the child does receive "Child ID Response" but parent misses the
        // 15.4 ack from child. If the "Child ID Response" does not make it
        // to the child, then the child will need to send a new "Child ID
        // Request" which will cause the parent to switch to using long
        // address mode for source address matching.

        mSourceMatchController.SetSrcMatchAsShort(aChild, true);

#if !OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE

        // When `CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE` is
        // disabled, all fragment frames of a larger message are
        // sent even if the transmission of an earlier fragment fail.
        // Note that `GetIndirectTxSuccess() tracks the tx success of
        // the entire message to the child, while `txError = aError`
        // represents the error status of the last fragment frame
        // transmission.

        if (!aChild.GetIndirectTxSuccess() && (txError == kErrorNone))
        {
            txError = kErrorFailed;
        }
#endif

        if (!aFrame.IsEmpty())
        {
            IgnoreError(aFrame.GetDstAddr(macDest));
            Get<MeshForwarder>().LogMessage(MeshForwarder::kMessageTransmit, *message, txError, &macDest);
        }

        Get<MeshForwarder>().mCounters.UpdateOnTxDone(*message, aChild.GetIndirectTxSuccess());

        if (message->GetIndirectTxChildMask().Has(childIndex))
        {
            message->GetIndirectTxChildMask().Remove(childIndex);
            mSourceMatchController.DecrementMessageCount(aChild);
        }

        message->InvokeTxCallback(txError);

#if OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE
        if (aFrame.IsEmpty())
        {
            aChild.GetMacAddress(macDest);
        }

        Get<HistoryTracker::Local>().RecordTxMessage(*message, macDest, txError == kErrorNone);
#endif
        Get<MeshForwarder>().RemoveMessageIfNoPendingTx(*message);
    }

    UpdateIndirectMessage(aChild);

exit:
    if (mEnabled)
    {
        ClearMessagesForRemovedChildren();
    }
}

void IndirectSender::ClearMessagesForRemovedChildren(void)
{
    for (Child &child : Get<ChildTable>().Iterate(Child::kInStateAnyExceptValidOrRestoring))
    {
        if (child.GetIndirectMessageCount() == 0)
        {
            continue;
        }

        ClearAllMessagesForSleepyChild(child);
    }
}

bool IndirectSender::AcceptSupervisionMessage(const Message &aMessage)
{
    return aMessage.GetType() == Message::kTypeSupervision;
}

#endif // OPENTHREAD_FTD

// GAMA
#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
#if OPENTHREAD_FTD
void IndirectSender::AddMessageForSleepyRouter(Message &aMessage, Router &aRouter)
{
    // TODO: check the Router is, in-fact, sleepy
    OT_ASSERT(aRouter.IsCslSynchronized());

    aMessage.SetPendingForRouter(aRouter.GetRouterId());
    aRouter.IncrementIndirectMessageCount();
    RequestMessageUpdate(aRouter);
}

Error IndirectSender::RemoveMessageFromSleepyRouter(Message &aMessage, Router &aRouter)
{
    Error error = kErrorNone;
    // TODO: verify that the Router in the message is the one im operating for (i guess?)
    VerifyOrExit(aMessage.IsPendingForRouter(aRouter.GetRouterId()));

    // clear the pending for Router field
    aMessage.ClearPendingForRouter();
    aRouter.DecrementIndirectMessageCount();

    RequestMessageUpdate(aRouter);
exit:
    return error;
}

void IndirectSender::HandleSentFrameToCslRouter(const Mac::TxFrame &aFrame,
                                                const FrameContext &aContext,
                                                Error               aError,
                                                Router             &aRouter)
{
    Message *message    = aRouter.GetIndirectMessage();
    uint16_t nextOffset = aContext.mMessageNextOffset;

    VerifyOrExit(mEnabled);

    VerifyOrExit(nextOffset != 0);

    switch (aError)
    {
    case kErrorNone:
        break;

    case kErrorNoAck:
    case kErrorChannelAccessFailure:
    case kErrorAbort:

        aRouter.SetIndirectTxSuccess(false);

#if OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE
        if (message != nullptr)
        {
            nextOffset = message->GetLength();
        }
#endif
        break;

    default:
        OT_ASSERT(false);
    }

    if ((message != nullptr) && (nextOffset < message->GetLength()))
    {
        aRouter.SetIndirectFragmentOffset(nextOffset);
        mCslTxScheduler.Update();
        ExitNow();
    }

    if (message != nullptr)
    {
        Error        txError = aError;
        Mac::Address macDest;

        aRouter.SetIndirectMessage(nullptr);
        aRouter.GetLinkInfo().AddMessageTxStatus(aRouter.GetIndirectTxSuccess());

#if !OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE

        if (!aRouter.GetIndirectTxSuccess() && (txError == kErrorNone))
        {
            txError = kErrorFailed;
        }
#endif

        if (!aFrame.IsEmpty())
        {
            IgnoreError(aFrame.GetDstAddr(macDest));
            Get<MeshForwarder>().LogMessage(MeshForwarder::kMessageTransmit, *message, txError, &macDest);
        }
        // same, not sure because we are accessing the MeshForwarder mCounters
        Get<MeshForwarder>().mCounters.UpdateOnTxDone(*message, aRouter.GetIndirectTxSuccess());

        if (message->IsPendingForRouter(aRouter.GetRouterId()))
        {
            message->ClearPendingForRouter();
            aRouter.DecrementIndirectMessageCount();
        }

        message->InvokeTxCallback(txError);

#if OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE
        if (aFrame.IsEmpty())
        {
            aRouter.GetMacAddress(macDest);
        }

        Get<HistoryTracker::Local>().RecordTxMessage(*message, macDest, txError == kErrorNone);
#endif
        Get<MeshForwarder>().RemoveMessageIfNoPendingTx(*message);
    }

    UpdateIndirectMessage(aRouter);

exit:
    return;
}

void IndirectSender::UpdateIndirectMessage(Router &aRouter)
{
    Message *message = FindQueuedMessageForSleepyRouter(aRouter, AcceptAnyMessage);

    aRouter.SetWaitingForMessageUpdate(false);
    aRouter.SetIndirectMessage(message);
    aRouter.SetIndirectFragmentOffset(0);
    aRouter.SetIndirectTxSuccess(true);

#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Update();
#endif

    if (message != nullptr)
    {
        Mac::Address routerAddress;

        aRouter.GetMacAddress(routerAddress);
        Get<MeshForwarder>().LogMessage(MeshForwarder::kMessagePrepareIndirect, *message, kErrorNone, &routerAddress);
    }
}

Message *IndirectSender::FindQueuedMessageForSleepyRouter(Router &aRouter, MessageChecker aChecker)
{
    Message *match = nullptr;

    for (Message &message : Get<MeshForwarder>().mSendQueue)
    {
        if (message.IsPendingForRouter(aRouter.GetRouterId()) && aChecker(message))
        {
            match = &message;
            break;
        }
    }

    return match;
}

void IndirectSender::RequestMessageUpdate(Router &aRouter)
{
    Message *curMessage = aRouter.GetIndirectMessage();
    Message *newMessage;

    VerifyOrExit(!aRouter.IsWaitingForMessageUpdate());

    newMessage = FindQueuedMessageForSleepyRouter(aRouter, AcceptAnyMessage);

    VerifyOrExit(curMessage != newMessage);

    VerifyOrExit(aRouter.GetIndirectFragmentOffset() == 0);

    UpdateIndirectMessage(aRouter);

exit:
    return;
}

// GAMA: mirrors `ClearAllMessagesForSleepyChild()` above. Called from `Mle::RemoveNeighbor()` when a
// Router-peer is removed (link timeout, role change, etc.) so any message still queued indirectly for
// it (`Message::IsPendingForRouter()`) is released instead of leaking forever in `mSendQueue` -- see the
// "purge scenario" analysis in docs/sleepy-router/DESIGN_LOG.md, section 6.
void IndirectSender::ClearAllMessagesForSleepyRouter(Router &aRouter)
{
    VerifyOrExit(aRouter.GetIndirectMessageCount() > 0);

    for (Message &message : Get<MeshForwarder>().mSendQueue)
    {
        if (message.IsPendingForRouter(aRouter.GetRouterId()))
        {
            message.ClearPendingForRouter();
        }

        Get<MeshForwarder>().RemoveMessageIfNoPendingTx(message);
    }

    aRouter.SetIndirectMessage(nullptr);
    aRouter.ResetIndirectMessageCount();

#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE
    mCslTxScheduler.Update();
#endif

exit:
    return;
}

#endif // OPENTHREAD_FTD

Error IndirectSender::RemoveMessageFromSleepyParent(Message &aMessage)
{
    Error error = kErrorNone;
    // get the pointer to the parent
    Parent &parent = Get<Mle::Mle>().GetParent();
    VerifyOrExit(aMessage.IsPendingForParent(), error = kErrorNotFound);
    // mark the message to be removed
    aMessage.ClearPendingForParent();
    RequestMessageUpdate(parent);

exit:
    return error;
}

// GAMA
void IndirectSender::AddMessageForSleepyParent(Message &aMessage, Parent &aParent)
{
    OT_ASSERT(aParent.IsCslSynchronized());

    aMessage.SetPendingForParent();
    RequestMessageUpdate(aParent);
}

// GAMA
Message *IndirectSender::FindQueuedMessageForSleepyParent(Parent &aParent, MessageChecker aChecker)
{
    OT_UNUSED_VARIABLE(aParent);
    Message *match = nullptr;

    for (Message &message : Get<MeshForwarder>().mSendQueue)
    {
        if (message.IsPendingForParent() && aChecker(message))
        {
            match = &message;
            break;
        }
    }

    return match;
}

// GAMA
void IndirectSender::UpdateIndirectMessage(Parent &aParent)
{
    Message *message = FindQueuedMessageForSleepyParent(aParent, AcceptAnyMessage);

    aParent.SetWaitingForMessageUpdate(false);
    aParent.SetIndirectMessage(message);
    aParent.SetIndirectFragmentOffset(0);
    aParent.SetIndirectTxSuccess(true);
    mCslTxScheduler.Update();
}

// GAMA
void IndirectSender::RequestMessageUpdate(Parent &aParent)
{
    Message *curMessage = aParent.GetIndirectMessage();
    Message *newMessage;

    VerifyOrExit(!aParent.IsWaitingForMessageUpdate());

    newMessage = FindQueuedMessageForSleepyParent(aParent, AcceptAnyMessage);

    VerifyOrExit(curMessage != newMessage);
    VerifyOrExit(aParent.GetIndirectFragmentOffset() == 0);

    UpdateIndirectMessage(aParent);

exit:
    return;
}

// GAMA
void IndirectSender::HandleSentFrameToCslParent(const Mac::TxFrame &aFrame,
                                                const FrameContext &aContext,
                                                Error               aError,
                                                Parent             &aParent)
{
    Message *message    = aParent.GetIndirectMessage();
    uint16_t nextOffset = aContext.mMessageNextOffset;

    VerifyOrExit(mEnabled);

    VerifyOrExit(nextOffset != 0);

    switch (aError)
    {
    case kErrorNone:
        break;

    case kErrorNoAck:
    case kErrorChannelAccessFailure:
    case kErrorAbort:

        aParent.SetIndirectTxSuccess(false);

#if OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE
        if (message != nullptr)
        {
            nextOffset = message->GetLength();
        }
#endif
        break;

    default:
        OT_ASSERT(false);
    }

    if ((message != nullptr) && (nextOffset < message->GetLength()))
    {
        aParent.SetIndirectFragmentOffset(nextOffset);
        mCslTxScheduler.Update();
        ExitNow();
    }

    if (message != nullptr)
    {
        Error        txError = aError;
        Mac::Address macDest;

        aParent.SetIndirectMessage(nullptr);
        aParent.GetLinkInfo().AddMessageTxStatus(aParent.GetIndirectTxSuccess());

#if !OPENTHREAD_CONFIG_DROP_MESSAGE_ON_FRAGMENT_TX_FAILURE

        if (!aParent.GetIndirectTxSuccess() && (txError == kErrorNone))
        {
            txError = kErrorFailed;
        }
#endif

        if (!aFrame.IsEmpty())
        {
            IgnoreError(aFrame.GetDstAddr(macDest));
            Get<MeshForwarder>().LogMessage(MeshForwarder::kMessageTransmit, *message, txError, &macDest);
        }
        // same, not sure because we are accessing the MeshForwarder mCounters
        Get<MeshForwarder>().mCounters.UpdateOnTxDone(*message, aParent.GetIndirectTxSuccess());

        if (message->IsPendingForParent())
        {
            message->ClearPendingForParent();
        }

        message->InvokeTxCallback(txError);

#if OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE
        if (aFrame.IsEmpty())
        {
            aParent.GetMacAddress(macDest);
        }

        Get<HistoryTracker::Local>().RecordTxMessage(*message, macDest, txError == kErrorNone);
#endif
        Get<MeshForwarder>().RemoveMessageIfNoPendingTx(*message);
    }

    UpdateIndirectMessage(aParent);

exit:
    return;
}

Error IndirectSender::PrepareFrameForCslNeighbor(Mac::TxFrame &aFrame,
                                                 FrameContext &aContext,
                                                 CslNeighbor  &aCslNeighbor)
{
    Error          error = kErrorNone;
    Message       *message;
    Ip6::Header    ip6Header;
    Mac::Addresses macAddrs;
    uint16_t       directTxOffset;

    // both Child and Router can now be CslNeighbors
    // i should probably if-else the Child - Router case (?)
    VerifyOrExit(mEnabled, error = kErrorAbort);

    aCslNeighbor.GetMacAddress(macAddrs.mDestination);

    message = aCslNeighbor.GetIndirectMessage();

    if ((message == nullptr) || (message->GetType() == Message::kTypeSupervision))
    {
        Get<MessageFramer>().PrepareEmptyFrame(aFrame, macAddrs.mDestination, /* aAckRequest */ true);
        aContext.mMessageNextOffset = (message == nullptr) ? 0 : message->GetLength();

        ExitNow();
    }

    VerifyOrExit(message->GetType() == Message::kTypeIp6);

    // Determine the MAC source and destination addresses.

    IgnoreError(message->Read(0, ip6Header));

    Get<MessageFramer>().DetermineMacSourceAddress(ip6Header.GetSource(), macAddrs);

    if (ip6Header.GetDestination().IsLinkLocalUnicast())
    {
        macAddrs.mDestination.SetExtendedFromIid(ip6Header.GetDestination().GetIid());
    }

    // Prepare the data frame from previous child's indirect offset.

    directTxOffset = message->GetOffset();
    message->SetOffset(aCslNeighbor.GetIndirectFragmentOffset());

    aContext.mMessageNextOffset = Get<MessageFramer>().PrepareFrame(aFrame, *message, macAddrs);

    message->SetOffset(directTxOffset);

    // Set `FramePending` if there are more queued messages (excluding
    // the current one being sent out) for the child (note `> 1` check).
    // The case where the current message itself requires fragmentation
    // is already checked and handled in the above `PrepareFrame` call.

    if (aCslNeighbor.GetIndirectMessageCount() > 1)
    {
        aFrame.SetFramePending(true);
    }

exit:
    return error;
}

void IndirectSender::HandleSentFrameToCslNeighbor(const Mac::TxFrame &aFrame,
                                                  const FrameContext &aContext,
                                                  Error               aError,
                                                  CslNeighbor        &aCslNeighbor)
{
    // non castare ciecamente a child, controlla se c'è nella table
#if OPENTHREAD_FTD
    // se sto instradando verso un child okay
    if (Get<ChildTable>().Contains(aCslNeighbor))
    {
        HandleSentFrameToChild(aFrame, aContext, aError, static_cast<Child &>(aCslNeighbor));
    }
    else if (Get<RouterTable>().Contains(aCslNeighbor))
    {
        // instrado verso un router
        HandleSentFrameToCslRouter(aFrame, aContext, aError, static_cast<Router &>(aCslNeighbor));
    }
    else
    {
        // devo instradare verso il mio Parent
        HandleSentFrameToCslParent(aFrame, aContext, aError, static_cast<Parent &>(aCslNeighbor));
    }
#else
    // im a child so i should handle the sent frame for my Parent
    HandleSentFrameToCslParent(aFrame, aContext, aError, static_cast<Parent &>(aCslNeighbor));
#endif
}

#endif // OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE

#endif // OPENTHREAD_FTD || OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE

} // namespace ot
