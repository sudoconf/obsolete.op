/*

 Copyright (c) 2013, SMB Phone Inc.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 The views and conclusions contained in the software and documentation are those
 of the authors and should not be interpreted as representing official policies,
 either expressed or implied, of the FreeBSD Project.

 */

#include <openpeer/stack/internal/stack_FinderRelayChannel.h>
#include <openpeer/stack/internal/stack_Helper.h>
#include <openpeer/stack/internal/stack_Account.h>
#include <openpeer/stack/internal/stack_Stack.h>

#include <openpeer/stack/IPeer.h>

#include <openpeer/services/IHTTP.h>

#include <zsLib/Log.h>
#include <zsLib/helpers.h>

#include <zsLib/Stringize.h>

namespace openpeer { namespace stack { ZS_DECLARE_SUBSYSTEM(openpeer_stack) } }

namespace openpeer
{
  namespace stack
  {
    using zsLib::Stringize;
    using stack::message::IMessageHelper;

    namespace internal
    {
//      typedef zsLib::XML::Exceptions::CheckFailed CheckFailed;

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark (helpers)
      #pragma mark

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel
      #pragma mark

      //-----------------------------------------------------------------------
      FinderRelayChannel::FinderRelayChannel(
                                             IMessageQueuePtr queue,
                                             AccountPtr account
                                             ) :
        zsLib::MessageQueueAssociator(queue),
        mID(zsLib::createPUID()),
        mAccount(account)
      {
        ZS_LOG_DEBUG(log("created"))
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::init(IFinderRelayChannelDelegatePtr delegate)
      {
        mDefaultSubscription = Subscription::create(mThisWeak.lock(), delegate);

        step();
      }

      //-----------------------------------------------------------------------
      FinderRelayChannel::~FinderRelayChannel()
      {
        ZS_LOG_DEBUG(log("destroyed"))
        mThisWeak.reset();
        cancel();
      }

      //-----------------------------------------------------------------------
      FinderRelayChannelPtr FinderRelayChannel::convert(IFinderRelayChannelPtr channel)
      {
        return boost::dynamic_pointer_cast<FinderRelayChannel>(channel);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel => IFinderRelayChannel
      #pragma mark

      //-----------------------------------------------------------------------
      String FinderRelayChannel::toDebugString(IFinderRelayChannelPtr channel, bool includeCommaPrefix)
      {
        if (!channel) return String(includeCommaPrefix ? ", message incoming=(null)" : "message incoming=(null)");

        FinderRelayChannelPtr pThis = FinderRelayChannel::convert(channel);
        return pThis->getDebugValueString(includeCommaPrefix);
      }

      //-----------------------------------------------------------------------
      FinderRelayChannelPtr FinderRelayChannel::connect(
                                                        IFinderRelayChannelDelegatePtr delegate,
                                                        AccountPtr account,
                                                        IPAddress remoteFinderIP,
                                                        const char *localContextID,
                                                        const char *relayAccessToken,
                                                        const char *relayAccessSecretProof,
                                                        const char *encryptDataUsingEncodingPassphrase
                                                        )
      {
        //TODO
      }

      //-----------------------------------------------------------------------
      FinderRelayChannelPtr FinderRelayChannel::createIncoming(
                                                               IFinderRelayChannelDelegatePtr delegate,
                                                               AccountPtr account
                                                               )
      {
        //TODO
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::cancel()
      {
        ZS_LOG_DEBUG(log("cancel called"))

        AutoRecursiveLock lock(getLock());

        setState(SessionState_Shutdown);

        mAccount.reset();

        if (mMLSChannel) {
          mMLSChannel->cancel();
          mMLSChannel.reset();
        }

        mDefaultSubscription->cancel();
        mSubscriptions.clear();

        ZS_LOG_DEBUG(log("cancel complete"))
      }

      //-----------------------------------------------------------------------
      FinderRelayChannel::SessionStates FinderRelayChannel::getState(
                                                                     WORD *outLastErrorCode,
                                                                     String *outLastErrorReason
                                                                     ) const
      {
        AutoRecursiveLock lock(getLock());
        if (outLastErrorCode) *outLastErrorCode = mLastError;
        if (outLastErrorReason) *outLastErrorReason = mLastErrorReason;
        return mCurrentState;
      }

      //-----------------------------------------------------------------------
      IFinderRelayChannelSubscriptionPtr FinderRelayChannel::subscribe(IFinderRelayChannelDelegatePtr delegate)
      {
        ZS_LOG_TRACE(log("subscribe called"))

        AutoRecursiveLock lock(getLock());

        if (!delegate) return mDefaultSubscription;

        SubscriptionPtr subscription = Subscription::create(mThisWeak.lock(), delegate);

        if (SessionState_Pending != mCurrentState) {
          subscription->notifyStateChanged(mCurrentState);
        }

        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("subscription created after shutdown"))
          return subscription;
        }

        if (mMLSChannel) {
          if (mMLSChannel->getRemoteContextID().hasData()) {
            // have remote context ID, but have we set local context ID?
            if (mMLSChannel->getLocalContextID().isEmpty()) {
              subscription->notifylNeedsContext();
            }
          }

          // scope: notify about incoming messages
          {
            ULONG total = mMLSChannel->getTotalIncomingMessages();
            while (total > 0) {
              subscription->notifyIncomingMessage();
              --total;
            }
          }

          // scope: notify about message needing to be sent on the wire
          {
            ULONG total = mMLSChannel->getTotalPendingBuffersToSendOnWire();
            while (total > 0) {
              subscription->notifyBufferPendingToSendOnTheWire();
              --total;
            }
          }
        }

        mSubscriptions[subscription->getID()] = subscription;

        return subscription;
      }

      //-----------------------------------------------------------------------
      bool FinderRelayChannel::send(
                                    const BYTE *buffer,
                                    ULONG bufferSizeInBytes
                                    )
      {
        ZS_THROW_INVALID_ARGUMENT_IF(!buffer)
        ZS_THROW_INVALID_ARGUMENT_IF(0 == bufferSizeInBytes)

        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return SecureByteBlockPtr();
        }

        return mMLSChannel->send(buffer, bufferSizeInBytes);
      }

      //-----------------------------------------------------------------------
      SecureByteBlockPtr FinderRelayChannel::getNextIncomingMessage()
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return SecureByteBlockPtr();
        }

        return mMLSChannel->getNextIncomingMessage();
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::setIncomingContext(
                                                  const char *contextID,
                                                  const char *decryptUsingEncodingPassphrase
                                                  )
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return;
        }

        mMLSChannel->setLocalContextID(contextID);
        mMLSChannel->setReceiveKeyingDecoding(decryptUsingEncodingPassphrase);

        step();
      }

      //-----------------------d------------------------------------------------
      String FinderRelayChannel::getLocalContextID() const
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return String();
        }

        return mMLSChannel->getLocalContextID();
      }

      //-----------------------------------------------------------------------
      String FinderRelayChannel::getRemoteContextID() const
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return String();
        }

        return mMLSChannel->getRemoteContextID();
      }

      //-----------------------------------------------------------------------
      IPeerPtr FinderRelayChannel::getRemotePeer() const
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return IPeerPtr();
        }

        IPeerFilePublicPtr peerFilePublic = mMLSChannel->getRemoteReferencedPeerFilePublic();

        if (!peerFilePublic) {
          ZS_LOG_WARNING(Detail, log("no remote peer file reference known yet"))
          return IPeerPtr();
        }

        AccountPtr account = mAccount.lock();
        if (!account) {
          ZS_LOG_WARNING(Detail, log("account is gone thus peer cannot be known"))
          return IPeerPtr();
        }

        return IPeer::create(mAccount.lock(), peerFilePublic);
      }

      //-----------------------------------------------------------------------
      IRSAPublicKeyPtr FinderRelayChannel::getRemotePublicKey() const
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return IRSAPublicKeyPtr();
        }

        return mMLSChannel->getRemotePublicKey();
      }

      //-----------------------------------------------------------------------
      SecureByteBlockPtr FinderRelayChannel::getNextPendingBufferToSendOnWire()
      {
        AutoRecursiveLock lock(getLock());

        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return SecureByteBlockPtr();
        }

        return mMLSChannel->getNextPendingBufferToSendOnWire();
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::notifyReceivedFromWire(
                                                      const BYTE *buffer,
                                                      ULONG bufferLengthInBytes
                                                      )
      {
        ZS_THROW_INVALID_ARGUMENT_IF(!buffer)
        ZS_THROW_INVALID_ARGUMENT_IF(0 != bufferLengthInBytes)

        AutoRecursiveLock lock(getLock());
        if (!mMLSChannel) {
          ZS_LOG_WARNING(Detail, log("MLS channel is gone"))
          return;
        }

        return mMLSChannel->notifyReceivedFromWire(buffer, bufferLengthInBytes);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel => IMessageLayerSecurityChannelDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FinderRelayChannel::onMessageLayerSecurityChannelStateChanged(
                                                                         IMessageLayerSecurityChannelPtr channel,
                                                                         IMessageLayerSecurityChannel::SessionStates state
                                                                         )
      {
        AutoRecursiveLock lock(getLock());

        step();
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::onMessageLayerSecurityChannelNeedDecodingPassphrase(IMessageLayerSecurityChannelPtr channel)
      {
        AutoRecursiveLock lock(getLock());

        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("received event after shutdown"))
          return;
        }

        for (SubscriptionMap::iterator iter = mSubscriptions.begin(); iter != mSubscriptions.end(); ++iter)
        {
          SubscriptionPtr subscription = (*iter).second.lock();
          ZS_THROW_BAD_STATE_IF(!subscription)

          subscription->notifylNeedsContext();
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::onMessageLayerSecurityChannelIncomingMessage(IMessageLayerSecurityChannelPtr channel)
      {
        AutoRecursiveLock lock(getLock());

        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("received event after shutdown"))
          return;
        }

        for (SubscriptionMap::iterator iter = mSubscriptions.begin(); iter != mSubscriptions.end(); ++iter)
        {
          SubscriptionPtr subscription = (*iter).second.lock();
          ZS_THROW_BAD_STATE_IF(!subscription)

          subscription->notifyIncomingMessage();
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::onMessageLayerSecurityChannelBufferPendingToSendOnTheWire(IMessageLayerSecurityChannelPtr channel)
      {
        AutoRecursiveLock lock(getLock());

        if (isShutdown()) {
          ZS_LOG_WARNING(Detail, log("received event after shutdown"))
          return;
        }

        for (SubscriptionMap::iterator iter = mSubscriptions.begin(); iter != mSubscriptions.end(); ++iter)
        {
          SubscriptionPtr subscription = (*iter).second.lock();
          ZS_THROW_BAD_STATE_IF(!subscription)

          subscription->notifyBufferPendingToSendOnTheWire();
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel => IFinderRelayChannel
      #pragma mark

      //-----------------------------------------------------------------------
      void FinderRelayChannel::notifySubscriptionGone(Subscription &subscription)
      {
        AutoRecursiveLock lock(getLock());

        SubscriptionID id = subscription.getID();

        ZS_LOG_DEBUG(log("removing subscription") + ", subscription ID=" + Stringize<typeof(id)>(id).string())

        SubscriptionMap::iterator found = mSubscriptions.find(subscription.getID());
        if (found == mSubscriptions.end()) {
          ZS_LOG_WARNING(Debug, log("subscription being destroyed already gone (likely okay)"))
          return;
        }

        mSubscriptions.erase(found);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel  => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      RecursiveLock &FinderRelayChannel::getLock() const
      {
        return mLock;
      }

      //-----------------------------------------------------------------------
      String FinderRelayChannel::log(const char *message) const
      {
        return String("FinderRelayChannel [" + Stringize<typeof(mID)>(mID).string() + "] " + message);
      }

      //-----------------------------------------------------------------------
      String FinderRelayChannel::getDebugValueString(bool includeCommaPrefix) const
      {
        AutoRecursiveLock lock(getLock());
        bool firstTime = !includeCommaPrefix;

        return Helper::getDebugValue("relay channel id", Stringize<typeof(mID)>(mID).string(), firstTime) +
               Helper::getDebugValue("state", toString(mCurrentState), firstTime) +
               Helper::getDebugValue("last error", 0 != mLastError ? Stringize<typeof(mLastError)>(mLastError).string() : String(), firstTime) +
               Helper::getDebugValue("last reason", mLastErrorReason, firstTime) +
               Helper::getDebugValue("account", mAccount.lock() ? String("true") : String(), firstTime) +
               Helper::getDebugValue("default subscription", mDefaultSubscription ? String("true") : String(), firstTime) +
               Helper::getDebugValue("subscriptions", mSubscriptions.size() > 0 ? Stringize<SubscriptionMap::size_type>(mSubscriptions.size()).string() : String(), firstTime) +
               IMessageLayerSecurityChannel::toDebugString(mMLSChannel);
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::setState(SessionStates state)
      {
        if (state != mCurrentState) {
          ZS_LOG_DEBUG(log("state changed") + ", state=" + toString(state) + ", old state=" + toString(mCurrentState))
          mCurrentState = state;
        }

        FinderRelayChannelPtr pThis = mThisWeak.lock();
        if (pThis) {
          for (SubscriptionMap::iterator iter = mSubscriptions.begin(); iter != mSubscriptions.end(); ++iter)
          {
            SubscriptionPtr subscription = (*iter).second.lock();
            ZS_THROW_BAD_STATE_IF(!subscription)

            subscription->notifyStateChanged(state);
          }
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::setError(WORD errorCode, const char *inReason)
      {
        String reason(inReason ? String(inReason) : String());
        if (reason.isEmpty()) {
          reason = IHTTP::toString(IHTTP::toStatusCode(errorCode));
        }

        if (0 != mLastError) {
          ZS_LOG_WARNING(Detail, log("error already set thus ignoring new error") + ", new error=" + Stringize<typeof(errorCode)>(errorCode).string() + ", new reason=" + reason + getDebugValueString())
          return;
        }

        mLastError = errorCode;
        mLastErrorReason = reason;

        ZS_LOG_WARNING(Detail, log("error set") + ", code=" + Stringize<typeof(mLastError)>(mLastError).string() + ", reason=" + mLastErrorReason + getDebugValueString())
      }
      
      //-----------------------------------------------------------------------
      void FinderRelayChannel::step()
      {
        if (isShutdown()) {
          ZS_LOG_DEBUG(log("step continue to shutdown"))
          cancel();
          return;
        }

        ZS_LOG_DEBUG(log("step") + getDebugValueString())

        // TODO

        setState(SessionState_Connected);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel::Subscription
      #pragma mark

      //-----------------------------------------------------------------------
      FinderRelayChannel::Subscription::Subscription(
                                                     FinderRelayChannelPtr outer,
                                                     IFinderRelayChannelDelegatePtr delegate
                                                     ) :
        mID(zsLib::createPUID()),
        mOuter(outer),
        mDelegate(delegate ? IFinderRelayChannelDelegateProxy::createWeak(IStackForInternal::queueDelegate(), delegate) : IFinderRelayChannelDelegatePtr())
      {
        ZS_LOG_DEBUG(log("created"))
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::init()
      {
      }

      //-----------------------------------------------------------------------
      FinderRelayChannel::Subscription::~Subscription()
      {
        mThisWeak.reset();

        AutoRecursiveLock lock(getLock());
        ZS_LOG_DEBUG(log("destroyed"))
        cancel();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel::Subscription => IFinderRelayChannelSubscription
      #pragma mark

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::cancel()
      {
        AutoRecursiveLock lock(getLock());

        mDelegate.reset();

        FinderRelayChannelPtr outer = mOuter.lock();
        if (outer) outer->notifySubscriptionGone(*this);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel::Subscription => friend FinderRelayChannel
      #pragma mark

      //-----------------------------------------------------------------------
      FinderRelayChannel::SubscriptionPtr FinderRelayChannel::Subscription::create(
                                                                                   FinderRelayChannelPtr outer,
                                                                                   IFinderRelayChannelDelegatePtr delegate
                                                                                   )
      {
        SubscriptionPtr pThis(new Subscription(outer, delegate));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::notifyStateChanged(IFinderRelayChannel::SessionStates state)
      {
        if (!mDelegate) return;

        FinderRelayChannelPtr outer = mOuter.lock();
        if (!outer) return;

        try {
          mDelegate->onFinderRelayChannelStateChanged(outer, state);
        } catch(IFinderRelayChannelDelegateProxy::Exceptions::DelegateGone &) {
          ZS_LOG_WARNING(Debug, log("delegate gone"))
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::notifylNeedsContext()
      {
        if (!mDelegate) return;

        FinderRelayChannelPtr outer = mOuter.lock();
        if (!outer) return;

        try {
          mDelegate->onFinderRelayChannelNeedsContext(outer);
        } catch(IFinderRelayChannelDelegateProxy::Exceptions::DelegateGone &) {
          ZS_LOG_WARNING(Debug, log("delegate gone"))
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::notifyIncomingMessage()
      {
        if (!mDelegate) return;

        FinderRelayChannelPtr outer = mOuter.lock();
        if (!outer) return;

        try {
          mDelegate->onFinderRelayChannelIncomingMessage(outer);
        } catch(IFinderRelayChannelDelegateProxy::Exceptions::DelegateGone &) {
          ZS_LOG_WARNING(Debug, log("delegate gone"))
        }
      }

      //-----------------------------------------------------------------------
      void FinderRelayChannel::Subscription::notifyBufferPendingToSendOnTheWire()
      {
        if (!mDelegate) return;

        FinderRelayChannelPtr outer = mOuter.lock();
        if (!outer) return;

        try {
          mDelegate->onFinderRelayChannelBufferPendingToSendOnTheWire(outer);
        } catch(IFinderRelayChannelDelegateProxy::Exceptions::DelegateGone &) {
          ZS_LOG_WARNING(Debug, log("delegate gone"))
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FinderRelayChannel::Subscription => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      RecursiveLock &FinderRelayChannel::Subscription::getLock() const
      {
        FinderRelayChannelPtr outer = mOuter.lock();
        if (outer) return outer->getLock();
        return mBogusLock;
      }

      //-----------------------------------------------------------------------
      String FinderRelayChannel::Subscription::log(const char *message) const
      {
        return String("FinderRelayChannel::Subscription [") + Stringize<typeof(mID)>(mID).string() + "] " + message;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark IMessageLayerSecurityChannel
      #pragma mark

      //-----------------------------------------------------------------------
      const char *IFinderRelayChannel::toString(SessionStates state)
      {
        switch (state)
        {
          case SessionState_Pending:      return "Pending";
          case SessionState_Connected:    return "Connected";
          case SessionState_Shutdown:     return "Shutdown";
        }
        return "UNDEFINED";
      }

      //-----------------------------------------------------------------------
      IFinderRelayChannelPtr IFinderRelayChannel::connect(
                                                          IFinderRelayChannelDelegatePtr delegate,        // can pass in IFinderRelayChannelDelegatePtr() if not interested in the events
                                                          AccountPtr account,
                                                          IPAddress remoteFinderIP,
                                                          const char *localContextID,
                                                          const char *relayAccessToken,
                                                          const char *relayAccessSecretProof,
                                                          const char *encryptDataUsingEncodingPassphrase
                                                          )
      {
        return internal::IFinderRelayChannelFactory::singleton().connect(delegate, account, remoteFinderIP, localContextID, relayAccessToken, relayAccessSecretProof, encryptDataUsingEncodingPassphrase);
      }

      //-----------------------------------------------------------------------
      IFinderRelayChannelPtr IFinderRelayChannel::createIncoming(
                                                                 IFinderRelayChannelDelegatePtr delegate, // can pass in IFinderRelayChannelDelegatePtr() if not interested in the events
                                                                 AccountPtr account
                                                                 )
      {
        return internal::IFinderRelayChannelFactory::singleton().createIncoming(delegate, account);
      }
    }
  }
}
