/*
 * Copyright (c) 2014-2019 Belledonne Communications SARL.
 *
 * This file is part of bzrtp.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <string.h>

#include "bzrtp/bzrtp.h"

#include "bctoolbox/crypto.h"
#include "cryptoUtils.h"
#include "packetParser.h"
#include "stateMachine.h"
#include "typedef.h"
#include "zidCache.h"

#define BZRTP_ERROR_INVALIDCHANNELCONTEXT 0x8001

/* local functions prototypes */
static int bzrtp_initChannelContext(bzrtpContext_t *zrtpContext,
                                    bzrtpChannelContext_t *zrtpChannelContext,
                                    uint32_t selfSSRC, uint8_t isMain);
static void
bzrtp_destroyChannelContext(bzrtpContext_t *zrtpContext,
                            bzrtpChannelContext_t *zrtpChannelContext);
static bzrtpChannelContext_t *getChannelContext(bzrtpContext_t *zrtpContext,
                                                uint32_t selfSSRC);
static uint8_t copyCryptoTypes(uint8_t destination[7], uint8_t source[7],
                               uint8_t size);

/*
 * Create context structure and initialise it
 *
 * @return The ZRTP engine context data
 *
 */
bzrtpContext_t *bzrtp_createBzrtpContext(void) {
  int i;
  /*** create and intialise the context structure ***/
  bzrtpContext_t *context = (bzrtpContext_t *)malloc(sizeof(bzrtpContext_t));
  memset(context, 0, sizeof(bzrtpContext_t));

  /* start the random number generator */
  context->RNGContext =
      bctbx_rng_context_new(); /* TODO: give a seed for the RNG? */
  /* set the DHM context to NULL, it will be created if needed when creating a
   * DHPart packet */
  context->keyAgreementContext = NULL;
  context->keyAgreementAlgo = ZRTP_UNSET_ALGO;

  /* set flags */
  context->isSecure = 0; /* start unsecure */
  context->peerSupportMultiChannel =
      0; /* peer does not support Multichannel by default */
  context->isInitialised = 0; /* will be set by bzrtp_initBzrtpContext */
#ifdef GOCLEAR_ENABLED
  context->selfAcceptGoClear = 0;
  context->peerAcceptGoClear = 0;
#endif /* GOCLEAR_ENABLED */

  /* set to NULL all callbacks pointer */
  context->zrtpCallbacks.bzrtp_statusMessage = NULL;
  context->zrtpCallbacks.bzrtp_sendData = NULL;
  context->zrtpCallbacks.bzrtp_srtpSecretsAvailable = NULL;
  context->zrtpCallbacks.bzrtp_startSrtpSession = NULL;
  context->zrtpCallbacks.bzrtp_contextReadyForExportedKeys = NULL;

  for (i = 1; i < ZRTP_MAX_CHANNEL_NUMBER; i++) {
    context->channelContext[i] = NULL;
  }

  /* get the list of crypto algorithms provided by the crypto module */
  /* this list may then be updated according to users settings */
  context->hc = bzrtpUtils_getAvailableCryptoTypes(ZRTP_HASH_TYPE,
                                                   context->supportedHash);
  context->cc = bzrtpUtils_getAvailableCryptoTypes(ZRTP_CIPHERBLOCK_TYPE,
                                                   context->supportedCipher);
  context->ac = bzrtpUtils_getAvailableCryptoTypes(ZRTP_AUTHTAG_TYPE,
                                                   context->supportedAuthTag);
  context->kc = bzrtpUtils_getAvailableCryptoTypes(
      ZRTP_KEYAGREEMENT_TYPE, context->supportedKeyAgreement);
  context->sc =
      bzrtpUtils_getAvailableCryptoTypes(ZRTP_SAS_TYPE, context->supportedSas);

  /* initialise cached secret buffer to null */
  context->zidCache = NULL; /* a pointer to the sqlite3 db accessor, can be NULL
                               if running cacheless */
  context->zidCacheMutex =
      NULL; /* a pointer to a mutex provided by the environment to lock database
               during access, ignored if NULL */
  context->zuid = 0;
  context->peerBzrtpVersion = 0;
  context->selfURI = NULL;
  context->cachedSecret.rs1 = NULL;
  context->cachedSecret.rs1Length = 0;
  context->cachedSecret.rs2 = NULL;
  context->cachedSecret.rs2Length = 0;
  context->cachedSecret.pbxsecret = NULL;
  context->cachedSecret.pbxsecretLength = 0;
  context->cachedSecret.auxsecret = NULL;
  context->cachedSecret.auxsecretLength = 0;
  context->cacheMismatchFlag = 0;
  context->peerPVS = 0;

  /* initialise transient shared auxiliary secret buffer */
  context->transientAuxSecret = NULL;
  context->transientAuxSecretLength = 0;

  /* initialise key buffers */
  context->ZRTPSess = NULL;
  context->ZRTPSessLength = 0;
  context->exportedKey = NULL;
  context->exportedKeyLength = 0;

  /* initialise MTU to default value */
  context->mtu = BZRTP_DEFAULT_MTU;

  return context;
}

/**
 * @brief Set the pointer allowing cache access
 *
 * @param[in,out]	context			The ZRTP context we're dealing
 * with
 * @param[in]		zidCache		Used by internal function to
 * access cache: turn into a sqlite3 pointer if cache is enabled
 * @param[in]   	selfURI			Local URI used for this
 * communication, needed to perform cache operation, NULL terminated string,
 * duplicated by this function
 * @param[in]   	peerURI			Peer URI used for this
 * communication, needed to perform cache operation, NULL terminated string,
 * duplicated by this function
 *
 * @return 0 or BZRTP_CACHE_SETUP(if cache is populated by this call) on
 * success, error code otherwise
 */
int bzrtp_setZIDCache(bzrtpContext_t *context, void *zidCache,
                      const char *selfURI, const char *peerURI) {
#ifdef ZIDCACHE_ENABLED
  /* is zrtp context valid */
  if (context == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* zidCache pointer is actually a pointer to sqlite3 db, store it in context
   */
  context->zidCache = (sqlite3 *)zidCache;
  if (context->selfURI != NULL) {
    free(context->selfURI);
  }
  context->selfURI = strdup(selfURI);

  if (context->peerURI != NULL) {
    free(context->peerURI);
  }
  context->peerURI = strdup(peerURI);

  /* and init the cache(create needed tables if they don't exist) */
  return bzrtp_initCache_lock(context->zidCache, context->zidCacheMutex);
#else  /* ZIDCACHE_ENABLED */
  return BZRTP_ERROR_CACHEDISABLED;
#endif /* ZIDCACHE_ENABLED */
}

/**
 * @brief Set the pointer allowing cache access,
 * this version of the function get a mutex to lock the cache when accessing it
 *
 * @param[in,out]	context			The ZRTP context we're dealing
 * with
 * @param[in]		zidCache		Used by internal function to
 * access cache: turn into a sqlite3 pointer if cache is enabled
 * @param[in]		selfURI			Local URI used for this
 * communication, needed to perform cache operation, NULL terminated string,
 * duplicated by this function
 * @param[in]		peerURI			Peer URI used for this
 * communication, needed to perform cache operation, NULL terminated string,
 * duplicated by this function
 * @param[in]		zidCacheMutex		Points to a mutex used to lock
 * zidCache database access
 *
 * @return 0 or BZRTP_CACHE_SETUP(if cache is populated by this call) on
 * success, error code otherwise
 */
int bzrtp_setZIDCache_lock(bzrtpContext_t *context, void *zidCache,
                           const char *selfURI, const char *peerURI,
                           bctbx_mutex_t *zidCacheMutex) {
#ifdef ZIDCACHE_ENABLED
  /* is zrtp context valid */
  if (context == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  context->zidCacheMutex = zidCacheMutex;

  /* have the non lockable function finish the job */
  return bzrtp_setZIDCache(context, zidCache, selfURI, peerURI);
#else  /* ZIDCACHE_ENABLED */
  return BZRTP_ERROR_CACHEDISABLED;
#endif /* ZIDCACHE_ENABLED */
}

/**
 * @brief Perform some initialisation which can't be done without some callback
 *functions: This function is called once per session when the first channel is
 *created. It must be called after the cache access pointer have been set
 *	- Get ZID from cache or generate a random ZID
 *	- Initialise the first channel
 *
 *	@param[in] 	context		The context to initialise
 *	@param[in]	selfSSRC	SSRC of the first channel
 * @return 0 on success
 */
// ZRTP 채널 초기 준비
int bzrtp_initBzrtpContext(bzrtpContext_t *context, uint32_t selfSSRC) {

  /* is zrtp context valid */
  if (context == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* initialise self ZID. Randomly generated if no ZID is found in cache or no
   * cache found */
  bzrtp_getSelfZID_lock(context->zidCache, context->selfURI, context->selfZID,
                        context->RNGContext, context->zidCacheMutex);
  context->isInitialised = 1;

  /* In the case where we supporte a post quantum algorithm,
   * we need to supporte AES3 and SHA512 */
  int i = 0;
  while (i < context->kc &&
         !bzrtp_isPostQuantum(context->supportedKeyAgreement[i])) {
    i++;
  }
  if (i < context->kc) {
    int j = 0;
    if (context->cc < 7) {
      while (context->supportedCipher[j] != ZRTP_CIPHER_AES3 &&
             j < context->cc) {
        j++;
      }
      if (j == context->cc) {
        context->supportedCipher[context->cc] = ZRTP_CIPHER_AES3;
        context->cc++;
      }
    }
    j = 0;
    if (context->hc < 7) {
      while (context->supportedHash[j] != ZRTP_HASH_S512 && j < context->hc) {
        j++;
      }
      if (j == context->hc) {
        context->supportedHash[context->hc] = ZRTP_HASH_S512;
        context->hc++;
      }
    }
  }

  /* allocate 1 channel context, set all the others pointers to NULL */
  context->channelContext[0] =
      (bzrtpChannelContext_t *)malloc(sizeof(bzrtpChannelContext_t));
  memset(context->channelContext[0], 0, sizeof(bzrtpChannelContext_t));
  return bzrtp_initChannelContext(context, context->channelContext[0], selfSSRC,
                                  1);
}

/*
 * Free memory of context structure to a channel, if all channels are freed,
 * free the global zrtp context
 * @param[in]	context		Context hosting the channel to be
 * destroyed.(note: the context zrtp context itself is destroyed with the last
 * channel)
 * @param[in]	selfSSRC	The SSRC identifying the channel to be destroyed
 *
 * @return the number of channel still active in this ZRTP context
 *
 */
int bzrtp_destroyBzrtpContext(bzrtpContext_t *context, uint32_t selfSSRC) {
  int i;
  int validChannelsNumber = 0;

  if (context == NULL) {
    return 0;
  }

  /* Find the channel to be destroyed, destroy it and check if we have anymore
   * valid channels */
  for (i = 0; i < ZRTP_MAX_CHANNEL_NUMBER; i++) {
    if (context->channelContext[i] != NULL) {
      if (context->channelContext[i]->selfSSRC == selfSSRC) {
        bzrtp_destroyChannelContext(context, context->channelContext[i]);
        context->channelContext[i] = NULL;
      } else {
        validChannelsNumber++;
      }
    }
  }

  if (validChannelsNumber > 0) {
    return validChannelsNumber; /* we have more valid channels, keep the zrtp
                                   context */
  }

  /* We have no more channel, destroy the zrtp context */
  /* key agreement context shall already been destroyed after s0 computation,
   * but just in case */
  if (context->keyAgreementContext != NULL) {
    if (bzrtp_isKem(context->keyAgreementAlgo)) {
      bzrtp_destroyKEMContext(
          (bzrtp_KEMContext_t *)context->keyAgreementContext);
    } else if (context->keyAgreementAlgo == ZRTP_KEYAGREEMENT_X255 ||
               context->keyAgreementAlgo == ZRTP_KEYAGREEMENT_X448) {
      bctbx_DestroyECDHContext(
          (bctbx_ECDHContext_t *)context->keyAgreementContext);
    } else if (context->keyAgreementAlgo == ZRTP_KEYAGREEMENT_DH2k ||
               context->keyAgreementAlgo == ZRTP_KEYAGREEMENT_DH3k) {
      bctbx_DestroyDHMContext(
          (bctbx_DHMContext_t *)context->keyAgreementContext);
    }
    context->keyAgreementContext = NULL;
    context->keyAgreementAlgo = ZRTP_UNSET_ALGO;
  }

  /* Destroy keys and secrets */
  /* rs1, rs2, pbxsecret and auxsecret shall already been destroyed, just in
   * case */
  if (context->cachedSecret.rs1 != NULL) {
    bzrtp_DestroyKey(context->cachedSecret.rs1, context->cachedSecret.rs1Length,
                     context->RNGContext);
    free(context->cachedSecret.rs1);
    context->cachedSecret.rs1 = NULL;
  }
  if (context->cachedSecret.rs2 != NULL) {
    bzrtp_DestroyKey(context->cachedSecret.rs2, context->cachedSecret.rs2Length,
                     context->RNGContext);
    free(context->cachedSecret.rs2);
    context->cachedSecret.rs2 = NULL;
  }
  if (context->cachedSecret.auxsecret != NULL) {
    bzrtp_DestroyKey(context->cachedSecret.auxsecret,
                     context->cachedSecret.auxsecretLength,
                     context->RNGContext);
    free(context->cachedSecret.auxsecret);
    context->cachedSecret.auxsecret = NULL;
  }
  if (context->cachedSecret.pbxsecret != NULL) {
    bzrtp_DestroyKey(context->cachedSecret.pbxsecret,
                     context->cachedSecret.pbxsecretLength,
                     context->RNGContext);
    free(context->cachedSecret.pbxsecret);
    context->cachedSecret.pbxsecret = NULL;
  }

  if (context->ZRTPSess != NULL) {
    bzrtp_DestroyKey(context->ZRTPSess, context->ZRTPSessLength,
                     context->RNGContext);
    free(context->ZRTPSess);
    context->ZRTPSess = NULL;
  }

  if (context->exportedKey != NULL) {
    bzrtp_DestroyKey(context->exportedKey, context->exportedKeyLength,
                     context->RNGContext);
    free(context->exportedKey);
    context->ZRTPSess = NULL;
  }

  free(context->selfURI);
  free(context->peerURI);

  /* transient shared auxiliary secret */
  if (context->transientAuxSecret != NULL) {
    bzrtp_DestroyKey(context->transientAuxSecret,
                     context->transientAuxSecretLength, context->RNGContext);
    free(context->transientAuxSecret);
    context->transientAuxSecret = NULL;
  }

  /* destroy the RNG context at the end because it may be needed to destroy some
   * keys */
  bctbx_rng_context_free(context->RNGContext);
  context->RNGContext = NULL;
  free(context);
  return 0;
}

/*
 * @brief Allocate a function pointer to the callback function identified by his
 * id
 * @param[in,out]	context				The zrtp context to set
 * the callback function
 * @param[in] 		cbs 				A structure containing
 * all the callbacks to supply.
 *
 * @return 0 on success
 */
int bzrtp_setCallbacks(bzrtpContext_t *context, const bzrtpCallbacks_t *cbs) {
  /* is zrtp context valid */
  if (context == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  context->zrtpCallbacks = *cbs;
  return 0;
}

/**
 * @brief Add a channel to an existing context, this can be done only if the
 * first channel has concluded a DH key agreement
 *
 * @param[in,out]	zrtpContext	The zrtp context who will get the
 * additionnal channel. Must be in secure state.
 * @param[in]		selfSSRC	The SSRC given to the channel context
 *
 * @return 0 on succes, error code otherwise
 */
int bzrtp_addChannel(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {
  bzrtpChannelContext_t *zrtpChannelContext = NULL;
  int i = 0;

  /* is zrtp context valid */
  if (zrtpContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* context must be initialised(selfZID available) to enable the creation of an
   * additional channel */
  if (zrtpContext->isInitialised == 0) {
    return BZRTP_ERROR_CONTEXTNOTREADY;
  }

  /* get the first free channel context from ZRTP context and create a channel
   * context */
  while (i < ZRTP_MAX_CHANNEL_NUMBER && zrtpChannelContext == NULL) {
    if (zrtpContext->channelContext[i] == NULL) {
      int retval;
      zrtpChannelContext =
          (bzrtpChannelContext_t *)malloc(sizeof(bzrtpChannelContext_t));
      memset(zrtpChannelContext, 0, sizeof(bzrtpChannelContext_t));
      retval = bzrtp_initChannelContext(zrtpContext, zrtpChannelContext,
                                        selfSSRC, 0);
      if (retval != 0) {
        free(zrtpChannelContext);
        return retval;
      }
    } else {
      i++;
    }
  }

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_UNABLETOADDCHANNEL;
  }

  /* attach the created channel to the ZRTP context */
  zrtpContext->channelContext[i] = zrtpChannelContext;

  return 0;
}

/*
 * @brief Start the state machine of the specified channel
 *
 * @param[in,out]	zrtpContext			The ZRTP context hosting
 * the channel to be started
 * @param[in]		selfSSRC			The SSRC identifying the
 * channel to be started(will start sending Hello packets and listening for
 * some)
 *
 * @return			0 on succes, error code otherwise
 */
// ZRTP 핸드셰이크 시작
int bzrtp_startChannelEngine(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {
  bzrtpEvent_t initEvent;

  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* is this channel already started? */
  if (zrtpChannelContext->stateMachine != NULL) {
    return BZRTP_ERROR_CHANNELALREADYSTARTED;
  }

  /* if this is an additional channel(not channel 0), we must be sure that
   * channel 0 is already secured */
  if (zrtpChannelContext->isMainChannel == 0) {
    /* is ZRTP context able to add a channel (means channel 0 has already
     * performed the secrets generation) */
    if (zrtpContext->isSecure == 0) {
      return BZRTP_ERROR_CONTEXTNOTREADY;
    }
  }

  /* set the timer reference to 0 to force a message to be sent at first timer
   * tick */
  zrtpContext->timeReference = 0;

  /* start the engine by setting the state to init and calling it */
  // 상대머신 첫 상태
  zrtpChannelContext->stateMachine = state_discovery_init;

  /* create an INIT event to call the init state function which will create a
   * hello packet and start sending it */
  initEvent.eventType = BZRTP_EVENT_INIT;
  initEvent.bzrtpPacketString = NULL;
  initEvent.bzrtpPacketStringLength = 0;
  initEvent.zrtpContext = zrtpContext;
  initEvent.zrtpChannelContext = zrtpChannelContext;
  // INIT 이벤트 호출 → Hello 전송
  return zrtpChannelContext->stateMachine(initEvent);
}

/*
 * @brief Send the current time to a specified channel, it will check if it has
 * to trig some timer
 *
 * @param[in,out]	zrtpContext			The ZRTP context hosting
 * the channel
 * @param[in]		selfSSRC			The SSRC identifying the
 * channel
 * @param[in]		timeReference		The current time in ms
 *
 * @return			0 on succes, error code otherwise
 */
int bzrtp_iterate(bzrtpContext_t *zrtpContext, uint32_t selfSSRC,
                  uint64_t timeReference) {
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* update the context time reference used when arming timers */
  zrtpContext->timeReference = timeReference;

  if (zrtpChannelContext->timer.status == BZRTP_TIMER_ON) {
    if (zrtpChannelContext->timer.firingTime <=
        timeReference) { /* we must trig the timer */
      bzrtpEvent_t timerEvent;

      zrtpChannelContext->timer.firingCount++;

      /* create a timer event */
      timerEvent.eventType = BZRTP_EVENT_TIMER;
      timerEvent.bzrtpPacketString = NULL;
      timerEvent.bzrtpPacketStringLength = 0;
      timerEvent.bzrtpPacket = NULL;
      timerEvent.zrtpContext = zrtpContext;
      timerEvent.zrtpChannelContext = zrtpChannelContext;

      /* send it to the state machine*/
      if (zrtpChannelContext->stateMachine != NULL) {
        return zrtpChannelContext->stateMachine(timerEvent);
      }
    }
  }

  return 0;
}

/*
 * @brief Set the client data pointer in a channel context
 * This pointer is returned to the client by the callbacks function, used to
 * store associated contexts (RTP session)
 * @param[in,out]	zrtpContext		The ZRTP context we're dealing
 * with
 * @param[in]		selfSSRC		The SSRC identifying the channel
 * to be linked to the client Data
 * @param[in]		clientData		The clientData pointer, casted
 * to a (void *)
 *
 * @return 0 on success
 *
 */
int bzrtp_setClientData(bzrtpContext_t *zrtpContext, uint32_t selfSSRC,
                        void *clientData) {
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  zrtpChannelContext->clientData = clientData;

  return 0;
}

/*
 * @brief Process a received message
 *
 * @param[in,out]	zrtpContext				The ZRTP context
 * we're dealing with
 * @param[in]		selfSSRC				The SSRC
 * identifying the channel receiving the message
 * @param[in]		zrtpPacketString		The packet received
 * @param[in]		zrtpPacketStringLength	Length of the packet in bytes
 *
 * @return 	0 on success, errorcode otherwise
 */
// 상대가 먼저 보낸 Hello 패킷을 수신한 경우 -> 파싱 후 BZRTP_EVENT_MESSAGE로
// state machine 호출 -> 자동으로 Responder 경로 탑승
int bzrtp_processMessage(bzrtpContext_t *zrtpContext, uint32_t selfSSRC,
                         uint8_t *zrtpPacketString,
                         uint16_t zrtpPacketStringLength) {
  int retval;
  bzrtpPacket_t *zrtpPacket;
  bzrtpEvent_t event;

  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* check the context is initialised (we may receive packets before
   * initialisation is complete i.e. between channel initialisation and channel
   * start) */
  if (zrtpChannelContext->stateMachine == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT; /* drop the message */
  }

  /* first check the packet */
  uint8_t *incomingPacket = zrtpPacketString;
  uint16_t incomingPacketLength = zrtpPacketStringLength;
  zrtpPacket = bzrtp_packetCheck(&incomingPacket, &incomingPacketLength,
                                 zrtpChannelContext, &retval);
  if (retval != 0) {
    if (retval == BZRTP_PARSER_INFO_PACKETFRAGMENT) {
      /* fragment of incomplete packet incoming, just wait for the rest of it to
       * arrive*/
      return 0;
    }
    /*TODO: check the returned error code and do something or silent drop? */
    return retval;
  }

  /* Intercept error(Not managed yet) and ping zrtp packets */
  /* if we have a ping packet, just answer with a ping ACK and do not forward to
   * the state machine */
  if (zrtpPacket->messageType == MSGTYPE_PING) {
    bzrtpPacket_t *pingAckPacket = NULL;

    bzrtp_packetParser(zrtpContext, zrtpChannelContext, incomingPacket,
                       incomingPacketLength, zrtpPacket);
    /* store ping packet in the channel context as packet creator will need it
     * to create the pingACK */
    zrtpChannelContext->pingPacket = zrtpPacket;
    /* create the pingAck packet */
    pingAckPacket = bzrtp_createZrtpPacket(zrtpContext, zrtpChannelContext,
                                           MSGTYPE_PINGACK, &retval);
    if (retval == 0) {
      retval =
          bzrtp_packetBuild(zrtpContext, zrtpChannelContext, pingAckPacket);
      if (retval == 0 && zrtpContext->zrtpCallbacks.bzrtp_sendData !=
                             NULL) { /* send the packet */
        bzrtp_packetSetSequenceNumber(pingAckPacket,
                                      zrtpChannelContext->selfSequenceNumber);
        zrtpContext->zrtpCallbacks.bzrtp_sendData(
            zrtpChannelContext->clientData, pingAckPacket->packetString,
            pingAckPacket->messageLength + ZRTP_PACKET_OVERHEAD);
        zrtpChannelContext->selfSequenceNumber++;
      }
    }

    /* free packets and reset channel context storage */
    bzrtp_freeZrtpPacket(zrtpPacket);
    bzrtp_freeZrtpPacket(pingAckPacket);
    zrtpChannelContext->pingPacket = NULL;

    return retval;
  }

  /* build a packet event of it and send it to the state machine */
  event.eventType = BZRTP_EVENT_MESSAGE;
  event.bzrtpPacketString = incomingPacket;
  event.bzrtpPacketStringLength = incomingPacketLength;
  event.bzrtpPacket = zrtpPacket;
  event.zrtpContext = zrtpContext;
  event.zrtpChannelContext = zrtpChannelContext;

  return zrtpChannelContext->stateMachine(event);
}

/*
 * @brief Called by user when the SAS has been verified
 * update the cache(if any) to set the previously verified flag
 *
 * @param[in,out]	zrtpContext				The ZRTP context
 * we're dealing with
 */
void bzrtp_SASVerified(bzrtpContext_t *zrtpContext) {
  if (zrtpContext != NULL) {
    uint8_t pvsFlag = 1;
    const char *colNames[] = {"pvs"};
    uint8_t *colValues[] = {&pvsFlag};
    size_t colLength[] = {1};

    /* check if we must update the cache(delayed until sas verified in case of
     * cache mismatch) */
    if (zrtpContext->cacheMismatchFlag == 1) {
      zrtpContext->cacheMismatchFlag = 0;
      bzrtp_updateCachedSecrets(
          zrtpContext,
          zrtpContext->channelContext[0]); /* channel[0] is the only one in DHM
                                              mode, so the only one able to have
                                              a cache mismatch */
    }
    bzrtp_cache_write_lock(zrtpContext->zidCache, zrtpContext->zuid, "zrtp",
                           colNames, colValues, colLength, 1,
                           zrtpContext->zidCacheMutex);
  }
}

/*
 * @brief Called by user when the SAS has been set to unverified
 * update the cache(if any) to unset the previously verified flag
 *
 * @param[in,out]	zrtpContext				The ZRTP context
 * we're dealing with
 */
void bzrtp_resetSASVerified(bzrtpContext_t *zrtpContext) {
  if (zrtpContext != NULL) {
    uint8_t pvsFlag = 0;
    const char *colNames[] = {"pvs"};
    uint8_t *colValues[] = {&pvsFlag};
    size_t colLength[] = {1};
    bzrtp_cache_write_lock(zrtpContext->zidCache, zrtpContext->zuid, "zrtp",
                           colNames, colValues, colLength, 1,
                           zrtpContext->zidCacheMutex);
  }
}

/*
 * @brief  Allow client to compute an exported according to RFC section 4.5.2
 *		Check the context is ready(we already have a master exported key
 *and KDF context) and run KDF(master exported key, "Label", KDF_Context,
 *negotiated hash Length)
 *
 * @param[in]		zrtpContext		The ZRTP context we're dealing
 *with
 * @param[in]		label			Label used in the KDF
 * @param[in]		labelLength		Length of previous buffer
 * @param[out]		derivedKey		Buffer to store the derived key
 * @param[in,out]	derivedKeyLength	Length of previous
 *buffer(updated to fit actual length of data produced if too long)
 *
 * @return 0 on succes, error code otherwise
 */
int bzrtp_exportKey(bzrtpContext_t *zrtpContext, char *label,
                    size_t labelLength, uint8_t *derivedKey,
                    size_t *derivedKeyLength) {
  /* check we have s0 or exportedKey and KDFContext in channel[0] - export keys
   * is available only on channel 0 completion - see RFC 4.5.2 */
  bzrtpChannelContext_t *zrtpChannelContext = zrtpContext->channelContext[0];

  if (zrtpContext->peerBzrtpVersion ==
      0x010000) { /* older implementation of bzrtp had a bug in key export,
                     retrocompatibility is not supported anymore */
    /* We do not support anymore backward compatibility, just do nothing but
     * send an error message*/
    if (zrtpContext->zrtpCallbacks.bzrtp_statusMessage != NULL &&
        zrtpContext->zrtpCallbacks.bzrtp_messageLevel >=
            BZRTP_MESSAGE_ERROR) { /* use error level as we explicitely compile
                                      with no support for older version */
      zrtpContext->zrtpCallbacks.bzrtp_statusMessage(
          zrtpChannelContext->clientData, BZRTP_MESSAGE_ERROR,
          BZRTP_MESSAGE_PEERVERSIONOBSOLETE,
          "obsolete bzrtp version are not supported anymore");
    }
  } else { /* peer either use version 1.1 of BZRTP or another library, just
              stick to the RFC to create the export key */
    if ((zrtpChannelContext->s0 == NULL && zrtpContext->exportedKey) ||
        zrtpChannelContext->KDFContext == NULL) {
      return BZRTP_ERROR_INVALIDCONTEXT;
    }

    /* if we didn't already computed the master exported key, do it now */
    if (zrtpContext->exportedKey == NULL) {
      zrtpContext->exportedKeyLength = zrtpChannelContext->hashLength;
      zrtpContext->exportedKey =
          (uint8_t *)malloc(zrtpContext->exportedKeyLength * sizeof(uint8_t));
      bzrtp_keyDerivationFunction(
          zrtpChannelContext->s0, zrtpChannelContext->hashLength,
          (uint8_t *)"Exported key", 12, zrtpChannelContext->KDFContext,
          zrtpChannelContext->KDFContextLength, zrtpContext->exportedKeyLength,
          zrtpChannelContext->hmacFunction, zrtpContext->exportedKey);
    }

    /* We derive a maximum of hashLength bytes */
    if (*derivedKeyLength > zrtpChannelContext->hashLength) {
      *derivedKeyLength = zrtpChannelContext->hashLength;
    }

    bzrtp_keyDerivationFunction(
        zrtpContext->exportedKey, zrtpChannelContext->hashLength,
        (uint8_t *)label, labelLength, zrtpChannelContext->KDFContext,
        zrtpChannelContext->KDFContextLength, (uint8_t)(*derivedKeyLength),
        zrtpChannelContext->hmacFunction, derivedKey);
  }
  return 0;
}

/*
 * @brief Reset the retransmission timer of a given channel.
 * Packets will be sent again if appropriate:
 *  - when in responder role, zrtp engine only answer to packets sent by the
 * initiator.
 *  - if we are still in discovery phase, Hello or Commit packets will be
 * resent.
 *
 * @param[in,out]	zrtpContext				The ZRTP context
 * we're dealing with
 * @param[in]		selfSSRC				The SSRC
 * identifying the channel to reset
 *
 * return 0 on success, error code otherwise
 */

int bzrtp_resetRetransmissionTimer(bzrtpContext_t *zrtpContext,
                                   uint32_t selfSSRC) {
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }
  /* reset timer only when not in secure mode yet and for initiator(engine start
   * as initiator so if we call this function in discovery phase, it will reset
   * the timer */
  if ((zrtpChannelContext->isSecure == 0) &&
      (zrtpChannelContext->role == BZRTP_ROLE_INITIATOR)) {
    zrtpChannelContext->timer.status = BZRTP_TIMER_ON;
    zrtpChannelContext->timer.firingTime =
        0; /* be sure it will trigger at next call to bzrtp_iterate*/
    zrtpChannelContext->timer.firingCount =
        -1; /* -1 to count the initial packet and then retransmit the regular
               number of packets */
    /* reset timerStep to the base value */
    if ((zrtpChannelContext->timer.timerStep %
         NON_HELLO_BASE_RETRANSMISSION_STEP) == 0) {
      zrtpChannelContext->timer.timerStep = NON_HELLO_BASE_RETRANSMISSION_STEP;
    } else {
      zrtpChannelContext->timer.timerStep = HELLO_BASE_RETRANSMISSION_STEP;
    }
  }

  return 0;
}

/**
 * @brief Get the supported crypto types
 *
 * @param[in]		zrtpContext				The ZRTP context
 * we're dealing with
 * @param[in]		algoType				mapped to
 * defines, must be in [ZRTP_HASH_TYPE, ZRTP_CIPHERBLOCK_TYPE,
 * ZRTP_AUTHTAG_TYPE, ZRTP_KEYAGREEMENT_TYPE or ZRTP_SAS_TYPE]
 * @param[out]		supportedTypes			mapped to uint8_t value
 * of the 4 char strings giving the supported types as string according to rfc
 * section 5.1.2 to 5.1.6
 *
 * @return number of supported types, 0 on error
 */
uint8_t bzrtp_getSupportedCryptoTypes(bzrtpContext_t *zrtpContext,
                                      uint8_t algoType,
                                      uint8_t supportedTypes[7]) {
  if (zrtpContext == NULL) {
    return 0;
  }

  switch (algoType) {
  case ZRTP_HASH_TYPE:
    return copyCryptoTypes(supportedTypes, zrtpContext->supportedHash,
                           zrtpContext->hc);
  case ZRTP_CIPHERBLOCK_TYPE:
    return copyCryptoTypes(supportedTypes, zrtpContext->supportedCipher,
                           zrtpContext->cc);
  case ZRTP_AUTHTAG_TYPE:
    return copyCryptoTypes(supportedTypes, zrtpContext->supportedAuthTag,
                           zrtpContext->ac);
  case ZRTP_KEYAGREEMENT_TYPE:
    return copyCryptoTypes(supportedTypes, zrtpContext->supportedKeyAgreement,
                           zrtpContext->kc);
  case ZRTP_SAS_TYPE:
    return copyCryptoTypes(supportedTypes, zrtpContext->supportedSas,
                           zrtpContext->sc);
  default:
    return 0;
  }
}

/**
 * @brief set the supported crypto types
 *
 * @param[in,out]	zrtpContext				The ZRTP context
 * we're dealing with
 * @param[in]		algoType				mapped to
 * defines, must be in [ZRTP_HASH_TYPE, ZRTP_CIPHERBLOCK_TYPE,
 * ZRTP_AUTHTAG_TYPE, ZRTP_KEYAGREEMENT_TYPE or ZRTP_SAS_TYPE]
 * @param[in]		supportedTypes			mapped to uint8_t value
 * of the 4 char strings giving the supported types as string according to rfc
 * section 5.1.2 to 5.1.6
 * @param[in]		supportedTypesCount		number of supported
 * crypto types
 */
int bzrtp_setSupportedCryptoTypes(bzrtpContext_t *zrtpContext, uint8_t algoType,
                                  uint8_t supportedTypes[7],
                                  uint8_t supportedTypesCount) {
  if (zrtpContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* if the context is already initialised, it's too late to set the supported
   * crypto algos */
  if (zrtpContext->isInitialised) {
    return BZRTP_ERROR_CONTEXTNOTREADY;
  }

  uint8_t implementedTypes[256];
  uint8_t implementedTypesCount;

  implementedTypesCount =
      bzrtpUtils_getAllAvailableCryptoTypes(algoType, implementedTypes);

  switch (algoType) {
  case ZRTP_HASH_TYPE:
    zrtpContext->hc = bzrtp_selectCommonAlgo(
        supportedTypes, supportedTypesCount, implementedTypes,
        implementedTypesCount, zrtpContext->supportedHash);
    bzrtp_addMandatoryCryptoTypesIfNeeded(algoType, zrtpContext->supportedHash,
                                          &zrtpContext->hc);
    break;
  case ZRTP_CIPHERBLOCK_TYPE:
    zrtpContext->cc = bzrtp_selectCommonAlgo(
        supportedTypes, supportedTypesCount, implementedTypes,
        implementedTypesCount, zrtpContext->supportedCipher);
    bzrtp_addMandatoryCryptoTypesIfNeeded(
        algoType, zrtpContext->supportedCipher, &zrtpContext->cc);
    break;
  case ZRTP_AUTHTAG_TYPE:
    zrtpContext->ac = bzrtp_selectCommonAlgo(
        supportedTypes, supportedTypesCount, implementedTypes,
        implementedTypesCount, zrtpContext->supportedAuthTag);
    bzrtp_addMandatoryCryptoTypesIfNeeded(
        algoType, zrtpContext->supportedAuthTag, &zrtpContext->ac);
    break;
  case ZRTP_KEYAGREEMENT_TYPE:
    zrtpContext->kc = bzrtp_selectCommonAlgo(
        supportedTypes, supportedTypesCount, implementedTypes,
        implementedTypesCount, zrtpContext->supportedKeyAgreement);
    bzrtp_addMandatoryCryptoTypesIfNeeded(
        algoType, zrtpContext->supportedKeyAgreement, &zrtpContext->kc);
    break;
  case ZRTP_SAS_TYPE:
    zrtpContext->sc = bzrtp_selectCommonAlgo(
        supportedTypes, supportedTypesCount, implementedTypes,
        implementedTypesCount, zrtpContext->supportedSas);
    bzrtp_addMandatoryCryptoTypesIfNeeded(algoType, zrtpContext->supportedSas,
                                          &zrtpContext->sc);
    break;
  }

  return 0;
}

int bzrtp_setFlags(bzrtpContext_t *zrtpContext, uint8_t flagId, uint8_t value) {
#ifdef GOCLEAR_ENABLED
  if (zrtpContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  switch (flagId) {
  case BZRTP_SELF_ACCEPT_GOCLEAR:
    if (value == 0) {
      zrtpContext->selfAcceptGoClear = 0;
    } else {
      zrtpContext->selfAcceptGoClear = 1;
    }
    return 0;
  default:
    return BZRTP_ERROR_INVALIDARGUMENT;
  }
#else
  return BZRTP_ERROR_GOCLEARDISABLED;
#endif /* GOCLEAR_ENABLED */
}

/**
 * @brief Set the peer hello hash given by signaling to a ZRTP channel
 *
 * @param[in,out]	zrtpContext
 * The ZRTP context we're dealing with
 * @param[in]		selfSSRC
 * The SSRC identifying the channel
 * @param[out]		peerHelloHashHexString			A NULL
 * terminated string containing the hexadecimal form of the hash received in
 * signaling, may contain ZRTP version as header.
 * @param[in]		peerHelloHashHexStringLength	Length of hash string
 * (shall be at least 64 as the hash is a SHA256 so 32 bytes, more if it
 * contains the version header)
 *
 * @return 	0 on success, errorcode otherwise
 */
int bzrtp_setPeerHelloHash(bzrtpContext_t *zrtpContext, uint32_t selfSSRC,
                           uint8_t *peerHelloHashHexString,
                           size_t peerHelloHashHexStringLength) {
  size_t i = 0;
  uint8_t *hexHashString = NULL;
  size_t hexHashStringLength = peerHelloHashHexStringLength;
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* parse the given peerHelloHash, it may formatted <version> <hexa string
   * hash> or just <hexa string hash> */
  /* just ignore anything(we do not care about version number) before a ' ' if
   * found */
  while (hexHashString == NULL && i < peerHelloHashHexStringLength) {
    if (strncmp((const char *)(peerHelloHashHexString + i), " ", 1) == 0) {
      hexHashString = peerHelloHashHexString + i + 1;
      hexHashStringLength -= (i + 1);
    }
    i++;
  }
  if (hexHashString ==
      NULL) { /* there were no ' ' in the input string so it shall be the hex
                 hash only without version number */
    hexHashString = peerHelloHashHexString;
  }

  /* allocate memory to store the hash, length will be hexa string length/2 -
   * check it wasn't already allocated */
  if (zrtpChannelContext->peerHelloHash) {
    free(zrtpChannelContext->peerHelloHash);
  }
  zrtpChannelContext->peerHelloHash =
      (uint8_t *)malloc(hexHashStringLength / 2 * sizeof(uint8_t));

  /* convert to uint8 the hex string */
  bctbx_str_to_uint8(zrtpChannelContext->peerHelloHash, hexHashString,
                     hexHashStringLength);

  /* Do we already have the peer Hello packet, if yes, check it match the hash
   */
  if (zrtpChannelContext->peerPackets[HELLO_MESSAGE_STORE_ID] != NULL) {
    uint8_t computedPeerHelloHash[32];
    /* compute hash using implicit hash function: SHA256, skip packet header in
     * the packetString buffer as the hash must be computed on message only */
    bctbx_sha256(
        zrtpChannelContext->peerPackets[HELLO_MESSAGE_STORE_ID]->packetString +
            ZRTP_PACKET_HEADER_LENGTH,
        zrtpChannelContext->peerPackets[HELLO_MESSAGE_STORE_ID]->messageLength,
        32, computedPeerHelloHash);

    /* check they are the same */
    if (memcmp(computedPeerHelloHash, zrtpChannelContext->peerHelloHash, 32) !=
        0) {
      /* a session already started on this channel but with a wrong Hello we
       * must reset and restart it */
      /* note: caller may decide to abort the ZRTP session */
      /* reset state Machine */
      zrtpChannelContext->stateMachine = NULL;

      /* set timer off */
      zrtpChannelContext->timer.status = BZRTP_TIMER_OFF;

      /* destroy and free the key buffers */
      bzrtp_DestroyKey(zrtpChannelContext->s0, zrtpChannelContext->hashLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->KDFContext,
                       zrtpChannelContext->KDFContextLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->mackeyi,
                       zrtpChannelContext->hashLength, zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->mackeyr,
                       zrtpChannelContext->hashLength, zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->zrtpkeyi,
                       zrtpChannelContext->cipherKeyLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->zrtpkeyr,
                       zrtpChannelContext->cipherKeyLength,
                       zrtpContext->RNGContext);

      free(zrtpChannelContext->s0);
      free(zrtpChannelContext->KDFContext);
      free(zrtpChannelContext->mackeyi);
      free(zrtpChannelContext->mackeyr);
      free(zrtpChannelContext->zrtpkeyi);
      free(zrtpChannelContext->zrtpkeyr);

      zrtpChannelContext->s0 = NULL;
      zrtpChannelContext->KDFContext = NULL;
      zrtpChannelContext->mackeyi = NULL;
      zrtpChannelContext->mackeyr = NULL;
      zrtpChannelContext->zrtpkeyi = NULL;
      zrtpChannelContext->zrtpkeyr = NULL;

      /* free the allocated buffers but not our self Hello packet */
      for (i = 0; i < PACKET_STORAGE_CAPACITY; i++) {
        if (i != HELLO_MESSAGE_STORE_ID) {
          bzrtp_freeZrtpPacket(zrtpChannelContext->selfPackets[i]);
          zrtpChannelContext->selfPackets[i] = NULL;
        }
        bzrtp_freeZrtpPacket(zrtpChannelContext->peerPackets[i]);
        zrtpChannelContext->peerPackets[i] = NULL;
      }

      /* destroy and free the srtp and sas struture */
      bzrtp_DestroyKey(zrtpChannelContext->srtpSecrets.selfSrtpKey,
                       zrtpChannelContext->srtpSecrets.selfSrtpKeyLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->srtpSecrets.selfSrtpSalt,
                       zrtpChannelContext->srtpSecrets.selfSrtpSaltLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->srtpSecrets.peerSrtpKey,
                       zrtpChannelContext->srtpSecrets.peerSrtpKeyLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey(zrtpChannelContext->srtpSecrets.peerSrtpSalt,
                       zrtpChannelContext->srtpSecrets.peerSrtpSaltLength,
                       zrtpContext->RNGContext);
      bzrtp_DestroyKey((uint8_t *)zrtpChannelContext->srtpSecrets.sas,
                       zrtpChannelContext->srtpSecrets.sasLength,
                       zrtpContext->RNGContext);

      free(zrtpChannelContext->srtpSecrets.selfSrtpKey);
      free(zrtpChannelContext->srtpSecrets.selfSrtpSalt);
      free(zrtpChannelContext->srtpSecrets.peerSrtpKey);
      free(zrtpChannelContext->srtpSecrets.peerSrtpSalt);
      free(zrtpChannelContext->srtpSecrets.sas);
      for (i = 0; i < 3; i++) {
        bctbx_free(zrtpChannelContext->srtpSecrets.incorrectSas[i]);
        zrtpChannelContext->srtpSecrets.incorrectSas[i] = NULL;
      }

      /* re-initialise srtpSecrets structure */
      zrtpChannelContext->srtpSecrets.selfSrtpKey = NULL;
      zrtpChannelContext->srtpSecrets.selfSrtpSalt = NULL;
      zrtpChannelContext->srtpSecrets.peerSrtpKey = NULL;
      zrtpChannelContext->srtpSecrets.peerSrtpSalt = NULL;
      zrtpChannelContext->srtpSecrets.selfSrtpKeyLength = 0;
      zrtpChannelContext->srtpSecrets.selfSrtpSaltLength = 0;
      zrtpChannelContext->srtpSecrets.peerSrtpKeyLength = 0;
      zrtpChannelContext->srtpSecrets.peerSrtpSaltLength = 0;
      zrtpChannelContext->srtpSecrets.cipherAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->srtpSecrets.cipherKeyLength = 0;
      zrtpChannelContext->srtpSecrets.authTagAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->srtpSecrets.sasLength = 0;
      zrtpChannelContext->srtpSecrets.hashAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->srtpSecrets.keyAgreementAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->srtpSecrets.sasAlgo = ZRTP_UNSET_ALGO;

      /* reset choosen algo and their functions */
      zrtpChannelContext->hashAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->cipherAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->authTagAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->keyAgreementAlgo = ZRTP_UNSET_ALGO;
      zrtpChannelContext->sasAlgo = ZRTP_UNSET_ALGO;

      bzrtp_updateCryptoFunctionPointers(zrtpChannelContext);

      /* restart channel */
      bzrtp_startChannelEngine(zrtpContext, selfSSRC);

      return BZRTP_ERROR_HELLOHASH_MISMATCH;
    }
  }
  return 0;
}

/**
 * @brief Get the self hello hash from ZRTP channel
 *
 * @param[in,out]	zrtpContext			The ZRTP context we're
 * dealing with
 * @param[in]		selfSSRC			The SSRC identifying the
 * channel
 * @param[out]		output				A NULL terminated string
 * containing the hexadecimal form of the hash received in signaling, contain
 * ZRTP version as header. Buffer must be allocated by caller.
 * @param[in]		outputLength		Length of output buffer, shall
 * be at least 70 : 5 chars for version, 64 for the hash itself, SHA256), NULL
 * termination
 *
 * @return 	0 on success, errorcode otherwise
 */
int bzrtp_getSelfHelloHash(bzrtpContext_t *zrtpContext, uint32_t selfSSRC,
                           uint8_t *output, size_t outputLength) {
  uint8_t helloHash[32];
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  /* check we have the Hello packet in context */
  if (zrtpChannelContext->selfPackets[HELLO_MESSAGE_STORE_ID] == NULL) {
    return BZRTP_ERROR_CONTEXTNOTREADY;
  }

  /* check output length : version length +' '+64 hex hash + null termination */
  if (outputLength < strlen(ZRTP_VERSION) + 1 + 64 + 1) {
    return BZRTP_ERROR_OUTPUTBUFFER_LENGTH;
  }

  /* compute hash using implicit hash function: SHA256, skip packet header in
   * the packetString buffer as the hash must be computed on message only */
  bctbx_sha256(
      zrtpChannelContext->selfPackets[HELLO_MESSAGE_STORE_ID]->packetString +
          ZRTP_PACKET_HEADER_LENGTH,
      zrtpChannelContext->selfPackets[HELLO_MESSAGE_STORE_ID]->messageLength,
      32, helloHash);

  /* add version header */
  strcpy((char *)output, ZRTP_VERSION);
  output[strlen(ZRTP_VERSION)] = ' ';

  /* convert hash to hex string and set it in the output buffer */
  bctbx_int8_to_str(output + strlen(ZRTP_VERSION) + 1, helloHash, 32);

  /* add NULL termination */
  output[strlen(ZRTP_VERSION) + 1 + 64] = '\0';

  return 0;
}

/**
 * @brief Set Auxiliary Secret for this channel(shall be used only on primary
 * audio channel) The given auxSecret is appended to any aux secret found in
 * ZIDcache This function must be called before reception of peerHello packet
 *
 * @param[in]		zrtpContext	The ZRTP context we're dealing with
 * @param[in]		auxSecret	A buffer holding the auxiliary shared
 * secret to use (see RFC 6189 section 4.3)
 * @param[in]		auxSecretLength	lenght of the previous buffer
 *
 * @return 0 on success, error code otherwise
 *
 * @note The auxiliary shared secret mechanic is used by LIMEv2 for encryption
 * security purposes but might be used for its original purpose in a regular
 * ZRTP session if it becomes necessary in the future, or by another encryption
 * engine for example. In that case the API will need an adaptation work.
 */
int bzrtp_setAuxiliarySharedSecret(bzrtpContext_t *zrtpContext,
                                   const uint8_t *auxSecret,
                                   size_t auxSecretLength) {
  if (zrtpContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  if (zrtpContext->channelContext[0] &&
      zrtpContext->channelContext[0]->peerPackets[HELLO_MESSAGE_STORE_ID] !=
          NULL) {
    return BZRTP_ERROR_CONTEXTNOTREADY;
  }

  /* allocate memory to store the secret - check it wasn't already allocated */
  if (zrtpContext->transientAuxSecret) {
    free(zrtpContext->transientAuxSecret);
  }
  zrtpContext->transientAuxSecret =
      (uint8_t *)malloc(auxSecretLength * sizeof(uint8_t));

  /* copy the aux secret and length */
  memcpy(zrtpContext->transientAuxSecret, auxSecret, auxSecretLength);
  zrtpContext->transientAuxSecretLength = auxSecretLength;

  return 0;
}

/**
 * @brief Get the ZRTP auxiliary shared secret mismatch status
 *
 * @param[in]		zrtpContext			The ZRTP context we're
 * dealing with
 * @return	BZRTP_AUXSECRET_MATCH on match, BZRTP_AUXSECRET_MISMATCH on
 * mismatch, BZRTP_AUXSECRET_UNSET if auxiliary shared secret is unused
 */
uint8_t bzrtp_getAuxiliarySharedSecretMismatch(bzrtpContext_t *zrtpContext) {
  return zrtpContext->channelContext[0]->srtpSecrets.auxSecretMismatch;
}

/**
 * @brief Get the channel status
 *
 * @param[in]		zrtpContext			The ZRTP context we're
 *dealing with
 * @param[in]		selfSSRC			The SSRC identifying the
 *channel
 *
 * @return	BZRTP_CHANNEL_NOTFOUND 		no channel matching this SSRC
 *doesn't exists in the zrtp context BZRTP_CHANNEL_INITIALISED	channel
 *initialised but not started BZRTP_CHANNEL_ONGOING		ZRTP key
 *exchange in ongoing BZRTP_CHANNEL_SECURE		Channel is secure
 *			BZRTP_CHANNEL_ERROR			An error occured
 *on this channel
 */
int bzrtp_getChannelStatus(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {

  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_CHANNEL_NOTFOUND;
  }

  if (zrtpChannelContext->stateMachine == NULL) {
    return BZRTP_CHANNEL_INITIALISED;
  }

  if (zrtpChannelContext->isSecure == 1) {
    return BZRTP_CHANNEL_SECURE;
  }
#ifdef GOCLEAR_ENABLED
  if (zrtpChannelContext->isClear == 1) {
    return BZRTP_CHANNEL_CLEAR;
  }
#endif /* GOCLEAR_ENABLED */
  return BZRTP_CHANNEL_ONGOING;
}

/* Local functions implementation */

/**
 * @brief Look in the given ZRTP context for a channel referenced with given
 * SSRC
 *
 * @param[in]	zrtpContext	The zrtp context which shall contain the channel
 * context we are looking for
 * @param[in]	selfSSRC	The SSRC identifying the channel context
 *
 * @return 		a pointer to the channel context, NULL if the context is
 * invalid or channel not found
 */
static bzrtpChannelContext_t *getChannelContext(bzrtpContext_t *zrtpContext,
                                                uint32_t selfSSRC) {
  int i;

  if (zrtpContext == NULL) {
    return NULL;
  }

  for (i = 0; i < ZRTP_MAX_CHANNEL_NUMBER; i++) {
    if (zrtpContext->channelContext[i] != NULL) {
      if (zrtpContext->channelContext[i]->selfSSRC == selfSSRC) {
        return zrtpContext->channelContext[i];
      }
    }
  }

  return NULL; /* found no channel with this SSRC */
}
/**
 * @brief Initialise the context of a channel and create and store the Hello
 * packet Initialise some vectors
 *
 * @param[in] 		zrtpContext			The zrtpContext hosting
 * this channel, needed to acces the RNG
 * @param[out]		zrtpChannelContext	The channel context to be
 * initialised
 * @param[in]		selfSSRC			The SSRC allocated to
 * this channel
 * @param[in]		isMain				This channel is channel
 * 0 or an additional channel
 *
 * @return	0 on success, error code otherwise
 */
static int bzrtp_initChannelContext(bzrtpContext_t *zrtpContext,
                                    bzrtpChannelContext_t *zrtpChannelContext,
                                    uint32_t selfSSRC, uint8_t isMain) {
  int i;
  int retval;
  bzrtpPacket_t *helloPacket;

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCHANNELCONTEXT;
  }

  zrtpChannelContext->clientData = NULL;

  /* the state machine is not started at the creation of the channel but on
   * explicit call to the start function */
  zrtpChannelContext->stateMachine = NULL;

  /* timer is off */
  zrtpChannelContext->timer.status = BZRTP_TIMER_OFF;
  zrtpChannelContext->timer.timerStep =
      HELLO_BASE_RETRANSMISSION_STEP; /* we must initialise the timeStep just in
                                         case the resettimer function is called
                                         between init and start */

  zrtpChannelContext->selfSSRC = selfSSRC;

  /* flags */
  zrtpChannelContext->isSecure = 0;
  zrtpChannelContext->isMainChannel = isMain;
#ifdef GOCLEAR_ENABLED
  zrtpChannelContext->isClear = 0;
  zrtpChannelContext->hasReceivedAGoClear = BZRTP_RECEPTION_UNKNOWN;
#endif /* GOCLEAR_ENABLED */

  /* initialise as initiator, switch to responder later if needed */
  zrtpChannelContext->role = BZRTP_ROLE_INITIATOR;

  /* create H0 (32 bytes random) and derive using implicit Hash(SHA256) H1,H2,H3
   */
  bctbx_rng_get(zrtpContext->RNGContext, zrtpChannelContext->selfH[0], 32);
  bctbx_sha256(zrtpChannelContext->selfH[0], 32, 32,
               zrtpChannelContext->selfH[1]);
  bctbx_sha256(zrtpChannelContext->selfH[1], 32, 32,
               zrtpChannelContext->selfH[2]);
  bctbx_sha256(zrtpChannelContext->selfH[2], 32, 32,
               zrtpChannelContext->selfH[3]);

  /* initialisation of packet storage */
  for (i = 0; i < PACKET_STORAGE_CAPACITY; i++) {
    zrtpChannelContext->selfPackets[i] = NULL;
    zrtpChannelContext->peerPackets[i] = NULL;
  }
  zrtpChannelContext->peerHelloHash = NULL;

  /* initialisation of fragmented packet reception */
  zrtpChannelContext->incomingFragmentedPacket.fragments = NULL;
  zrtpChannelContext->incomingFragmentedPacket.messageId = 0;
  zrtpChannelContext->incomingFragmentedPacket.packetString = NULL;

  /* initialise the self Sequence number to a random and peer to 0 */
  bctbx_rng_get(zrtpContext->RNGContext,
                (uint8_t *)&(zrtpChannelContext->selfSequenceNumber), 2);
  zrtpChannelContext->selfSequenceNumber &=
      0x0FFF; /* first 4 bits to zero in order to avoid reaching FFFF and
                 turning back to 0 */
  zrtpChannelContext
      ->selfSequenceNumber++; /* be sure it is not initialised to 0 */
  zrtpChannelContext->selfMessageSequenceNumber =
      zrtpChannelContext->selfSequenceNumber; /* message sequence number gets
                                                 the same initial value */
  zrtpChannelContext->peerSequenceNumber = 0;

  /* reset choosen algo and their functions */
  zrtpChannelContext->hashAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->cipherAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->authTagAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->keyAgreementAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->sasAlgo = ZRTP_UNSET_ALGO;

  bzrtp_updateCryptoFunctionPointers(zrtpChannelContext);

  /* initialise key buffers */
  zrtpChannelContext->s0 = NULL;
  zrtpChannelContext->KDFContext = NULL;
  zrtpChannelContext->KDFContextLength = 0;
  zrtpChannelContext->mackeyi = NULL;
  zrtpChannelContext->mackeyr = NULL;
  zrtpChannelContext->zrtpkeyi = NULL;
  zrtpChannelContext->zrtpkeyr = NULL;

  /* initialise srtpSecrets structure */
  zrtpChannelContext->srtpSecrets.selfSrtpKey = NULL;
  zrtpChannelContext->srtpSecrets.selfSrtpSalt = NULL;
  zrtpChannelContext->srtpSecrets.peerSrtpKey = NULL;
  zrtpChannelContext->srtpSecrets.peerSrtpSalt = NULL;
  zrtpChannelContext->srtpSecrets.selfSrtpKeyLength = 0;
  zrtpChannelContext->srtpSecrets.selfSrtpSaltLength = 0;
  zrtpChannelContext->srtpSecrets.peerSrtpKeyLength = 0;
  zrtpChannelContext->srtpSecrets.peerSrtpSaltLength = 0;
  zrtpChannelContext->srtpSecrets.cipherAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->srtpSecrets.cipherKeyLength = 0;
  zrtpChannelContext->srtpSecrets.authTagAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->srtpSecrets.sas = NULL;
  for (i = 0; i < 3; i++) {
    zrtpChannelContext->srtpSecrets.incorrectSas[i] = NULL;
  }
  zrtpChannelContext->srtpSecrets.sasLength = 0;
  zrtpChannelContext->srtpSecrets.hashAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->srtpSecrets.keyAgreementAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->srtpSecrets.sasAlgo = ZRTP_UNSET_ALGO;
  zrtpChannelContext->srtpSecrets.cacheMismatch = 0;
  zrtpChannelContext->srtpSecrets.auxSecretMismatch = BZRTP_AUXSECRET_UNSET;

  /* create the Hello packet and store it */
  helloPacket = bzrtp_createZrtpPacket(zrtpContext, zrtpChannelContext,
                                       MSGTYPE_HELLO, &retval);
  if (retval != 0) {
    return retval;
  }
  /* build the packet string and store the packet */
  if (bzrtp_packetBuild(zrtpContext, zrtpChannelContext, helloPacket) == 0) {
    zrtpChannelContext->selfPackets[HELLO_MESSAGE_STORE_ID] = helloPacket;
  } else {
    bzrtp_freeZrtpPacket(helloPacket);
    return retval;
  }

  return 0;
}

/**
 * @brief Destroy the context of a channel
 * Free allocated buffers, destroy keys
 *
 * @param[in] 		zrtpContext			The zrtpContext hosting
 * this channel, needed to acces the RNG
 * @param[in] 		zrtpChannelContext	The channel context to be
 * destroyed
 */
static void
bzrtp_destroyChannelContext(bzrtpContext_t *zrtpContext,
                            bzrtpChannelContext_t *zrtpChannelContext) {
  int i;

  /* check there is something to be freed */
  if (zrtpChannelContext == NULL) {
    return;
  }

  /* reset state Machine */
  zrtpChannelContext->stateMachine = NULL;

  /* set timer off */
  zrtpChannelContext->timer.status = BZRTP_TIMER_OFF;

  bzrtp_destroyKeyMaterial(zrtpContext, zrtpChannelContext);

  /* free the allocated buffers */
  for (i = 0; i < PACKET_STORAGE_CAPACITY; i++) {
    bzrtp_freeZrtpPacket(zrtpChannelContext->selfPackets[i]);
    bzrtp_freeZrtpPacket(zrtpChannelContext->peerPackets[i]);
    zrtpChannelContext->selfPackets[i] = NULL;
    zrtpChannelContext->peerPackets[i] = NULL;
  }
  free(zrtpChannelContext->peerHelloHash);
  zrtpChannelContext->peerHelloHash = NULL;

  /* free possible fragment management buffers */
  bctbx_list_free_with_data(
      zrtpChannelContext->incomingFragmentedPacket.fragments, bctbx_free);
  zrtpChannelContext->incomingFragmentedPacket.fragments = NULL;
  bctbx_free(zrtpChannelContext->incomingFragmentedPacket.packetString);
  zrtpChannelContext->incomingFragmentedPacket.packetString = NULL;

  /* free the channel context */
  free(zrtpChannelContext);
}

static uint8_t copyCryptoTypes(uint8_t destination[7], uint8_t source[7],
                               uint8_t size) {
  int i;

  for (i = 0; i < (size & 0x07); i++) {
    destination[i] = source[i];
  }
  return size;
}

const char *bzrtp_algoToString(uint8_t algo) {
  switch (algo) {
  case (ZRTP_UNSET_ALGO):
    return "unset";

  case (ZRTP_HASH_S256):
    return "SHA-256";
  case (ZRTP_HASH_S384):
    return "SHA-384";
  case (ZRTP_HASH_S512):
    return "SHA-512";
  case (ZRTP_HASH_N256):
    return "SHA3-256";
  case (ZRTP_HASH_N384):
    return "SHA3-384";

  case (ZRTP_CIPHER_AES1):
    return "AES-128";
  case (ZRTP_CIPHER_AES2):
    return "AES-192";
  case (ZRTP_CIPHER_AES3):
    return "AES-256";
  case (ZRTP_CIPHER_2FS1):
    return "TwoFish-128";
  case (ZRTP_CIPHER_2FS2):
    return "TwoFish-192";
  case (ZRTP_CIPHER_2FS3):
    return "TwoFish-256";

  case (ZRTP_AUTHTAG_HS32):
    return "HMAC-SHA1-32";
  case (ZRTP_AUTHTAG_HS80):
    return "HMAC-SHA1-80";
  case (ZRTP_AUTHTAG_SK32):
    return "Skein-32";
  case (ZRTP_AUTHTAG_SK64):
    return "Skein-64";
  case (ZRTP_AUTHTAG_GCM):
    return "GCM";

  case (ZRTP_KEYAGREEMENT_DH2k):
    return "DHM-2048";
  case (ZRTP_KEYAGREEMENT_EC25):
    return "ECDH-256";
  case (ZRTP_KEYAGREEMENT_DH3k):
    return "DHM-3072";
  case (ZRTP_KEYAGREEMENT_EC38):
    return "ECDH-384";
  case (ZRTP_KEYAGREEMENT_EC52):
    return "ECDH-521";
  case (ZRTP_KEYAGREEMENT_X255):
    return "X25519";
  case (ZRTP_KEYAGREEMENT_X448):
    return "X448";
  case (ZRTP_KEYAGREEMENT_K255):
    return "K25519";
  case (ZRTP_KEYAGREEMENT_K448):
    return "K448";
  case (ZRTP_KEYAGREEMENT_Prsh):
    return "PreShared";
  case (ZRTP_KEYAGREEMENT_Mult):
    return "MultiStream";
  case (ZRTP_KEYAGREEMENT_MLK1):
    return "MLKem-512";
  case (ZRTP_KEYAGREEMENT_MLK2):
    return "MLKem-768";
  case (ZRTP_KEYAGREEMENT_MLK3):
    return "MLKem-1024";
  case (ZRTP_KEYAGREEMENT_KYB1):
    return "Kyber-512";
  case (ZRTP_KEYAGREEMENT_KYB2):
    return "Kyber-768";
  case (ZRTP_KEYAGREEMENT_KYB3):
    return "Kyber-1024";
  case (ZRTP_KEYAGREEMENT_HQC1):
    return "HQC-128";
  case (ZRTP_KEYAGREEMENT_HQC2):
    return "HQC-192";
  case (ZRTP_KEYAGREEMENT_HQC3):
    return "HQC-256";
  case (ZRTP_KEYAGREEMENT_K255_MLK512):
    return "K25519-MLKem512";
  case (ZRTP_KEYAGREEMENT_K255_KYB512):
    return "K25519-Kyber512";
  case (ZRTP_KEYAGREEMENT_K255_HQC128):
    return "K25519-HQC128";
  case (ZRTP_KEYAGREEMENT_K448_MLK1024):
    return "K448-MLKem1024";
  case (ZRTP_KEYAGREEMENT_K448_KYB1024):
    return "K448-Kyber1024";
  case (ZRTP_KEYAGREEMENT_K448_HQC256):
    return "K448-HQC256";
  case (ZRTP_KEYAGREEMENT_K255_KYB512_HQC128):
    return "K25519-Kyber512-HQC128";
  case (ZRTP_KEYAGREEMENT_K448_KYB1024_HQC256):
    return "K448-Kyber1024-HQC256";

  case (ZRTP_SAS_B32):
    return "Base32";
  case (ZRTP_SAS_B256):
    return "PGP-WordList";

  default:
    return "Unknown Algo";
  }
}

int bzrtp_set_MTU(bzrtpContext_t *zrtpContext, size_t mtu) {
  if (zrtpContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }
  if (mtu > BZRTP_MINIMUM_MTU) {
    zrtpContext->mtu = mtu;
  } else {
    zrtpContext->mtu = BZRTP_MINIMUM_MTU;
  }
  return 0;
}

size_t bzrtp_get_MTU(bzrtpContext_t *zrtpContext) {
  if (zrtpContext == NULL) {
    return BZRTP_DEFAULT_MTU;
  }
  return zrtpContext->mtu;
}

int bzrtp_sendGoClear(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {
#ifdef GOCLEAR_ENABLED
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL && zrtpChannelContext->isSecure) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  bzrtpEvent_t goClearEvent;

  /* create a GoClear event */
  goClearEvent.eventType = BZRTP_EVENT_GOCLEAR;
  goClearEvent.bzrtpPacketString = NULL;
  goClearEvent.bzrtpPacketStringLength = 0;
  goClearEvent.bzrtpPacket = NULL;
  goClearEvent.zrtpContext = zrtpContext;
  goClearEvent.zrtpChannelContext = zrtpChannelContext;

  /* send it to the state machine*/
  if (zrtpChannelContext->stateMachine != NULL) {
    return zrtpChannelContext->stateMachine(goClearEvent);
  }

  return 0;
#else
  return BZRTP_ERROR_GOCLEARDISABLED;
#endif /* GOCLEAR_ENABLED */
}

int bzrtp_confirmGoClear(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {
#ifdef GOCLEAR_ENABLED
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  bzrtpEvent_t acceptGoClearEvent;

  zrtpChannelContext->timer.firingCount++;

  /* create a accept GoClear event */
  acceptGoClearEvent.eventType = BZRTP_EVENT_ACCEPT_GOCLEAR;
  acceptGoClearEvent.bzrtpPacketString = NULL;
  acceptGoClearEvent.bzrtpPacketStringLength = 0;
  acceptGoClearEvent.bzrtpPacket = NULL;
  acceptGoClearEvent.zrtpContext = zrtpContext;
  acceptGoClearEvent.zrtpChannelContext = zrtpChannelContext;

  /* send it to the state machine */
  if (zrtpChannelContext->stateMachine != NULL) {
    return zrtpChannelContext->stateMachine(acceptGoClearEvent);
  }

  return 0;
#else
  return BZRTP_ERROR_GOCLEARDISABLED;
#endif /* GOCLEAR_ENABLED */
}

int bzrtp_backToSecureMode(bzrtpContext_t *zrtpContext, uint32_t selfSSRC) {
#ifdef GOCLEAR_ENABLED
  /* get channel context */
  bzrtpChannelContext_t *zrtpChannelContext =
      getChannelContext(zrtpContext, selfSSRC);
  bzrtpEvent_t backToSecureEvent;

  if (zrtpChannelContext == NULL) {
    return BZRTP_ERROR_INVALIDCONTEXT;
  }

  zrtpChannelContext->timer.firingCount++;

  /* create a GoClear event */
  backToSecureEvent.eventType = BZRTP_EVENT_BACKTOSECURE;
  backToSecureEvent.bzrtpPacketString = NULL;
  backToSecureEvent.bzrtpPacketStringLength = 0;
  backToSecureEvent.bzrtpPacket = NULL;
  backToSecureEvent.zrtpContext = zrtpContext;
  backToSecureEvent.zrtpChannelContext = zrtpChannelContext;

  /* send it to the state machine */
  if (zrtpChannelContext->stateMachine != NULL) {
    return zrtpChannelContext->stateMachine(backToSecureEvent);
  }

  return 0;
#else
  return BZRTP_ERROR_GOCLEARDISABLED;
#endif /* GOCLEAR_ENABLED */
}
