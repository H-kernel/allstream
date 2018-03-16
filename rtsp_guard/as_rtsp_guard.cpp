/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2017, Live Networks, Inc.  All rights reserved
// A demo application, showing how to create and run a RTSP client (that can potentially receive multiple streams concurrently).
//
// NOTE: This code - although it builds a running application - is intended only to illustrate how to develop your own RTSP
// client application.  For a full-featured RTSP client application - with much more functionality, and many options - see
// "openRTSP": http://www.live555.com/openRTSP/
#include "stdafx.h"
#include "liveMedia.hh"
#include "RTSPCommon.hh"
#include "GroupsockHelper.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "as_rtsp_guard.h"
#include "RTSPCommon.hh"
#include "as_log.h"
#include "as_lock_guard.h"
#include "as_ini_config.h"
#include "as_timer.h"
#include "as_mem.h"



#if defined(__WIN32__) || defined(_WIN32)
extern "C" int initializeWinsockIfNecessary();
#endif

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}


// Implementation of "ASRtsp2SipClient":

ASRtsp2RtpChannel* ASRtsp2RtpChannel::createNew(u_int32_t ulEnvIndex,UsageEnvironment& env, char const* rtspURL,
                    int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ASRtsp2RtpChannel(ulEnvIndex,env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ASRtsp2RtpChannel::ASRtsp2RtpChannel(u_int32_t ulEnvIndex,UsageEnvironment& env, char const* rtspURL,
                 int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1) {
  m_ulEnvIndex = ulEnvIndex;
  m_bSupportsGetParameter = False;
  m_enStatus = AS_RTSP_STATUS_INIT;
  m_bObervser = NULL;
}

ASRtsp2RtpChannel::~ASRtsp2RtpChannel() {
}

int32_t ASRtsp2RtpChannel::open(uint32_t ulDuration,ASRtspStatusObervser* observer)
{
    m_bObervser = observer;
    return sendOptionsCommand(&ASRtsp2RtpChannel::continueAfterOPTIONS);
}
void    ASRtsp2RtpChannel::close()
{
    if (scs.session != NULL) {
        Boolean someSubsessionsWereActive = False;
        MediaSubsessionIterator iter(*scs.session);
        MediaSubsession* subsession;

        while ((subsession = iter.next()) != NULL) {
            if (subsession->sink != NULL) {
                someSubsessionsWereActive = True;
            }
        }

        if (someSubsessionsWereActive) {
            // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
            // Don't bother handling the response to the "TEARDOWN".
            sendTeardownCommand(*scs.session, continueAfterTeardown);
        }
    }
}

void ASRtsp2RtpChannel::play()
{
    if (AS_RTSP_STATUS_SETUP != m_enStatus){
        return;
    }
    Groupsock* rtpGroupsock = NULL;
    if(scs.session != NULL) {
        /* open the send rtp sik */
        Boolean someSubsessionsWereActive = False;
        MediaSubsessionIterator iter(*scs.session);
        MediaSubsession* subsession;

        while ((subsession = iter.next()) != NULL) {
            if (!strcmp(subsession->mediumName(), "video")) {
                subsession->sink = ASRtspCheckVideoSink::createNew(envir(), *subsession);
           }
            else if (!strcmp(subsession->mediumName(), "audio")){
                subsession->sink = ASRtspCheckAudioSink::createNew(envir(), *subsession);
           }
           else {
               subsession->sink = NULL;
               continue;
           }

            // perhaps use your own custom "MediaSink" subclass instead
            if (subsession->sink == NULL) {
                continue;
            }

            subsession->miscPtr = this; // a hack to let subsession handler functions get the "RTSPClient" from the subsession
            subsession->sink->startPlaying(*(subsession->readSource()),
                             subsessionAfterPlaying, subsession);
            // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
            if (subsession->rtcpInstance() != NULL) {
              subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, subsession);
            }
        }

    }

    /* send play command */
    // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
    if (scs.session->absStartTime() != NULL) {
        // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
        sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
    } else {
        scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
        sendPlayCommand(*scs.session, continueAfterPLAY);
    }
}


void    ASRtsp2RtpChannel::handle_after_options(int resultCode, char* resultString)
{
    if(0 != resultCode) {
        shutdownStream();
        return;
    }

    do {
        if (resultCode != 0) {
          delete[] resultString;
          break;
        }

        Boolean serverSupportsGetParameter = RTSPOptionIsSupported("GET_PARAMETER", resultString);
        delete[] resultString;
        SupportsGetParameter(serverSupportsGetParameter);
        sendDescribeCommand(continueAfterDESCRIBE);
        return;
    } while (0);

    // An unrecoverable error occurred with this stream.
    shutdownStream();
    return;
}
void    ASRtsp2RtpChannel::handle_after_describe(int resultCode, char* resultString)
{
    if(0 != resultCode) {
        shutdownStream();
        return;
    }
    do {
        if (resultCode != 0) {
          delete[] resultString;
          break;
        }

        m_strRtspSdp = resultString;
        // Create a media session object from this SDP description:
        scs.session = MediaSession::createNew(envir(), m_strRtspSdp.c_str());
        delete[] resultString; // because we don't need it anymore
        if (scs.session == NULL) {
          break;
        } else if (!scs.session->hasSubsessions()) {
          break;
        }

        // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
        // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
        // (Each 'subsession' will have its own data source.)
        scs.iter = new MediaSubsessionIterator(*scs.session);
        setupNextSubsession();

        return;
    } while (0);

    // An unrecoverable error occurred with this stream.
    shutdownStream();

    return;
}


void    ASRtsp2RtpChannel::handle_after_setup(int resultCode, char* resultString)
{
    if(0 != resultCode) {
        shutdownStream();
        return;
    }
    do {

        if (resultCode != 0) {
            break;
        }
    } while (0);
    delete[] resultString;

    // Set up the next subsession, if any:
    setupNextSubsession();
    return;
}
void    ASRtsp2RtpChannel::handle_after_play(int resultCode, char* resultString)
{

    Boolean success = False;
    if(0 != resultCode) {
        shutdownStream();
        return;
    }
    do {

        if (resultCode != 0) {
            break;
        }

        // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
        // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
        // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
        // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
        if (scs.duration > 0) {
            unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
            scs.duration += delaySlop;
            unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
            scs.streamTimerTask = envir().taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, this);
        }

        success = True;
        if(SupportsGetParameter()) {
            sendGetParameterCommand(*scs.session,continueAfterGET_PARAMETE, "", NULL);
        }

    } while (0);
    delete[] resultString;

    if (!success) {
        // An unrecoverable error occurred with this stream.
        shutdownStream();
    }
    return;
}
void    ASRtsp2RtpChannel::handle_after_teardown(int resultCode, char* resultString)
{
    shutdownStream(0);
    return;
}
void    ASRtsp2RtpChannel::handle_subsession_after_playing(MediaSubsession* subsession)
{
    // Begin by closing this subsession's stream:
    Medium::close(subsession->sink);
    subsession->sink = NULL;

    // Next, check whether *all* subsessions' streams have now been closed:
    MediaSession& session = subsession->parentSession();
    MediaSubsessionIterator iter(session);
    while ((subsession = iter.next()) != NULL) {
        if (subsession->sink != NULL) return; // this subsession is still active
    }

    // All subsessions' streams have now been closed, so shutdown the client:
    shutdownStream();
}

void ASRtsp2RtpChannel::setupNextSubsession() {

    scs.subsession = scs.iter->next();
    if (scs.subsession != NULL) {
        if (!scs.subsession->initiate()) {
            setupNextSubsession(); // give up on this subsession; go to the next one
        } else {

            if (scs.subsession->rtpSource() != NULL) {
            // Because we're saving the incoming data, rather than playing
            // it in real time, allow an especially large time threshold
            // (1 second) for reordering misordered incoming packets:
            unsigned const thresh = 1000000; // 1 second
            scs.subsession->rtpSource()->setPacketReorderingThresholdTime(thresh);

            // Set the RTP source's OS socket buffer size as appropriate - either if we were explicitly asked (using -B),
            // or if the desired FileSink buffer size happens to be larger than the current OS socket buffer size.
            // (The latter case is a heuristic, on the assumption that if the user asked for a large FileSink buffer size,
            // then the input data rate may be large enough to justify increasing the OS socket buffer size also.)
            int socketNum = scs.subsession->rtpSource()->RTPgs()->socketNum();
            unsigned curBufferSize = getReceiveBufferSize(envir(), socketNum);
            unsigned ulRecvBufSize = ASRtspGuardManager::instance().getRecvBufSize();
            if (ulRecvBufSize > curBufferSize) {
                (void)setReceiveBufferTo(envir(), socketNum, ulRecvBufSize);
              }
            }

            // Continue setting up this subsession, by sending a RTSP "SETUP" command:
            sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
        }
        return;
    }

    /* send the play by the control */

    return;
}

void ASRtsp2RtpChannel::shutdownStream(int exitCode) {

    // First, check whether any subsessions have still to be closed:
    if (scs.session != NULL) {
        Boolean someSubsessionsWereActive = False;
        MediaSubsessionIterator iter(*scs.session);
        MediaSubsession* subsession;

        while ((subsession = iter.next()) != NULL) {
            if (subsession->sink != NULL) {
                Medium::close(subsession->sink);
                subsession->sink = NULL;
                if (subsession->rtcpInstance() != NULL) {
                    subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
                }
                someSubsessionsWereActive = True;
            }
        }

        if (someSubsessionsWereActive) {
          // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
          // Don't bother handling the response to the "TEARDOWN".
          sendTeardownCommand(*scs.session, NULL);
        }
    }

    /* report the status */
    if(exitCode) {
        if (NULL != m_bObervser)
        {
            m_enStatus = AS_RTSP_STATUS_TEARDOWN;
            m_bObervser->NotifyStatus(AS_RTSP_STATUS_TEARDOWN);
        }
    }

    /* not close here ,it will be closed by the close URL */
    //Medium::close(rtspClient);
    // Note that this will also cause this stream's "ASRtsp2SipStreamState" structure to get reclaimed.

}



// Implementation of the RTSP 'response handlers':
void ASRtsp2RtpChannel::continueAfterOPTIONS(RTSPClient* rtspClient, int resultCode, char* resultString) {

    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;
    pAsRtspClient->handle_after_options(resultCode,resultString);
}

void ASRtsp2RtpChannel::continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {

    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;
    pAsRtspClient->handle_after_describe(resultCode,resultString);

}


void ASRtsp2RtpChannel::continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {

    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;
    pAsRtspClient->handle_after_setup(resultCode,resultString);
}

void ASRtsp2RtpChannel::continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;
    pAsRtspClient->handle_after_play(resultCode,resultString);
}

void ASRtsp2RtpChannel::continueAfterGET_PARAMETE(RTSPClient* rtspClient, int resultCode, char* resultString) {
    delete[] resultString;
}

void ASRtsp2RtpChannel::continueAfterTeardown(RTSPClient* rtspClient, int resultCode, char* resultString)
{
    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;

    pAsRtspClient->handle_after_teardown(resultCode, resultString);
    delete[] resultString;
}
// Implementation of the other event handlers:

void ASRtsp2RtpChannel::subsessionAfterPlaying(void* clientData) {
    MediaSubsession* subsession = (MediaSubsession*)clientData;
    RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);
    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)rtspClient;

    pAsRtspClient->handle_subsession_after_playing(subsession);
}

void ASRtsp2RtpChannel::subsessionByeHandler(void* clientData) {
    MediaSubsession* subsession = (MediaSubsession*)clientData;
    // Now act as if the subsession had closed:
    subsessionAfterPlaying(subsession);
}

void ASRtsp2RtpChannel::streamTimerHandler(void* clientData) {
    ASRtsp2RtpChannel* rtspClient = (ASRtsp2RtpChannel*)clientData;
    ASRtspCheckStreamState& scs = rtspClient->scs; // alias

    scs.streamTimerTask = NULL;

    // Shut down the stream:
    rtspClient->shutdownStream();
}




// Implementation of "ASRtsp2SipStreamState":

ASRtspCheckStreamState::ASRtspCheckStreamState()
  : iter(NULL), session(NULL), subsession(NULL),
    streamTimerTask(NULL), duration(0.0){
}

ASRtspCheckStreamState::~ASRtspCheckStreamState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}


ASRtspCheckVideoSink* ASRtspCheckVideoSink::createNew(UsageEnvironment& env, MediaSubsession& subsession) {
    return new ASRtspCheckVideoSink(env, subsession);
}

ASRtspCheckVideoSink::ASRtspCheckVideoSink(UsageEnvironment& env, MediaSubsession& subsession)
  : MediaSink(env),fSubsession(subsession) {
    fReceiveBuffer = (u_int8_t*)&fMediaBuffer[0];
    prefixSize = 0;

    fReceiveBuffer = (u_int8_t*)&fMediaBuffer[DUMMY_SINK_H264_STARTCODE_SIZE];
    fMediaBuffer[0] = 0x00;
    fMediaBuffer[1] = 0x00;
    fMediaBuffer[2] = 0x00;
    fMediaBuffer[3] = 0x01;
    prefixSize = DUMMY_SINK_H264_STARTCODE_SIZE;

    uint32_t rtpTimestampFrequency = fSubsession.rtpTimestampFrequency();
    uint32_t ulFPS = fSubsession.videoFPS();
    if (0 == rtpTimestampFrequency || 0 == ulFPS) {
        m_rtpTimestampdiff = H264_RTP_TIMESTAMP_FREQUE;
    }
    else
    {
        m_rtpTimestampdiff = rtpTimestampFrequency / ulFPS;
    }
    m_lastTS = 0;

}

ASRtspCheckVideoSink::~ASRtspCheckVideoSink() {
    fReceiveBuffer = NULL;
}

void ASRtspCheckVideoSink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned durationInMicroseconds) {
    ASRtspCheckVideoSink* sink = (ASRtspCheckVideoSink*)clientData;
    sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void ASRtspCheckVideoSink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {

    unsigned int size = frameSize;
    int  sendBytes = 0;
    uint32_t valid_len = frameSize;
    unsigned char NALU = fMediaBuffer[prefixSize];
    m_lastTS += m_rtpTimestampdiff;

    continuePlaying();
}

Boolean ASRtspCheckVideoSink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}

ASRtspCheckAudioSink* ASRtspCheckAudioSink::createNew(UsageEnvironment& env, MediaSubsession& subsession) {
  return new ASRtspCheckAudioSink(env, subsession);
}

ASRtspCheckAudioSink::ASRtspCheckAudioSink(UsageEnvironment& env, MediaSubsession& subsession)
  : MediaSink(env),fSubsession(subsession) {

    uint32_t rtpTimestampFrequency = fSubsession.rtpTimestampFrequency();
    uint32_t ulFPS = fSubsession.videoFPS();
    if (0 == rtpTimestampFrequency || 0 == ulFPS) {
        m_rtpTimestampdiff = G711_RTP_TIMESTAMP_FREQUE;
    }
    else
    {
        m_rtpTimestampdiff = rtpTimestampFrequency / ulFPS;
    }

    m_lastTS = 0;
}

ASRtspCheckAudioSink::~ASRtspCheckAudioSink() {
}

void ASRtspCheckAudioSink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned durationInMicroseconds) {
    ASRtspCheckAudioSink* sink = (ASRtspCheckAudioSink*)clientData;
    sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void ASRtspCheckAudioSink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {

    continuePlaying();
}

Boolean ASRtspCheckAudioSink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame((u_int8_t*)&fMediaBuffer[0], DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}



ASLensInfo::ASLensInfo()
{
    m_Status = AS_RTSP_CHECK_STATUS_WAIT;
    m_strCameraID = "";
    m_strStreamType = "";
    m_handle = NULL;
    m_time = 0;
}
ASLensInfo::~ASLensInfo()
{
}
void ASLensInfo::setLensInfo(std::string& strCameraID,std::string& strStreamType)
{
    m_strCameraID = strCameraID;
    m_strStreamType = strStreamType;
}

void ASLensInfo::check()
{

    if(AS_RTSP_CHECK_STATUS_END == m_Status )
    {
        return;
    }
    if(AS_RTSP_CHECK_STATUS_RUN == m_Status)
    {
        /* check the run time and break */
        time_t cur = time(NULL);
        if(cur > (m_time + RTSP_CLINET_RUN_DURATION))
        {
            stopRtspCheck();
        }
        return;
    }
    if(RTSP_CLINET_HANDLE_MAX <= ASRtspGuardManager::instance().getRtspHandleCount())
    {
        return;
    }

    /* start the lens check */

    if(AS_ERROR_CODE_OK != StartRtspCheck())
    {
        /* start fail */
        return ;
    }
    m_time = time(NULL);

    m_Status = AS_RTSP_CHECK_STATUS_RUN;
}

CHECK_STATUS ASLensInfo::Status()
{
    return m_Status;
}
void ASLensInfo::NotifyStatus(AS_RTSP_STATUS status)
{
}

int32_t ASLensInfo::StartRtspCheck()
{
    ASEvLiveHttpClient httpHandle;
    std::string strRtspUrl;
    int32_t nRet = httpHandle.send_live_url_request(m_strCameraID,m_strStreamType,strRtspUrl);
    if(nRet != AS_ERROR_CODE_OK)
    {
        return AS_ERROR_CODE_FAIL;
    }
    m_handle = ASRtspGuardManager::instance().openURL(strRtspUrl.c_str(),this);
    if(NULL == m_handle)
    {
        return AS_ERROR_CODE_FAIL;
    }
    return AS_ERROR_CODE_OK;
}
void    ASLensInfo::stopRtspCheck()
{
    if(NULL != m_handle)
    {
        ASRtspGuardManager::instance().closeURL(m_handle);
        m_handle = NULL;
    }
    return;
}


ASRtspCheckTask::ASRtspCheckTask()
{
    m_Status = AS_RTSP_CHECK_STATUS_WAIT;
}
ASRtspCheckTask::~ASRtspCheckTask()
{
}
void ASRtspCheckTask::setTaskInfo(std::string& strCheckID,std::string& strReportUrl)
{
    m_strCheckID   = strCheckID;
    m_strReportUrl = strReportUrl;
}
void ASRtspCheckTask::addCamera(std::string& strCameraID,std::string& strStreamTye)
{
    ASLensInfo* pLenInfo = NULL;
    pLenInfo = AS_NEW(pLenInfo);
    if(NULL == pLenInfo)
    {
        return;
    }
    pLenInfo->setLensInfo(strCameraID, strStreamTye);
    m_LensList.push_back(pLenInfo);
}
void ASRtspCheckTask::checkTask()
{
    if(AS_RTSP_CHECK_STATUS_END == m_Status )
    {
        return;
    }

    ASLensInfo* pLenInfo = NULL;
    bool bRunning = false;

    LENSINFOLISTITRT iter = m_LensList.begin();

    for(; iter != m_LensList.end();++iter)
    {
        pLenInfo = *iter;
        pLenInfo->check();
        if(AS_RTSP_CHECK_STATUS_END != pLenInfo->Status() )
        {
            bRunning = true;
        }
    }

    if(bRunning)
    {
        return;
    }

    //report the task info to the server,and end eth task


    m_Status = AS_RTSP_CHECK_STATUS_END;
}
CHECK_STATUS ASRtspCheckTask::TaskStatus()
{
    return m_Status;
}

ASEvLiveHttpClient::ASEvLiveHttpClient()
{
    m_pReq    = NULL;
    m_pBase   = NULL;
    m_pConn   = NULL;
    m_reqPath = "/";
    m_strRespMsg = "";
}
ASEvLiveHttpClient::~ASEvLiveHttpClient()
{
    if (NULL != m_pConn)
    {
        evhttp_connection_free(m_pConn);
    }
    if (NULL != m_pBase)
    {
        event_base_free(m_pBase);
    }
    m_pReq = NULL;
}
int32_t ASEvLiveHttpClient::send_live_url_request(std::string& strCameraID,
                                               std::string& strStreamType,std::string& strRtspUrl)
{
    std::string strLiveUrl = ASRtspGuardManager::instance().getLiveUrl();
    std::string strAppID   = ASRtspGuardManager::instance().getAppID();
    std::string strAppSecr = ASRtspGuardManager::instance().getAppSecret();
    std::string strAppKey  = ASRtspGuardManager::instance().getAppKey();
    std::string strSign    = "all stream";

    if(AS_ERROR_CODE_OK != open_http_by_url(strLiveUrl)) {
        return AS_ERROR_CODE_FAIL;
    }

    /* 1.build the request json message */

    cJSON* root = cJSON_CreateObject();

    cJSON_AddItemToObject(root, "appID", cJSON_CreateString(strAppID.c_str()));
    /* TODO : account ,how to set */
    cJSON_AddItemToObject(root, "account", cJSON_CreateString(strAppID.c_str()));
    /* TODO : sign ,sign */
    cJSON_AddItemToObject(root, "sign", cJSON_CreateString(strSign.c_str()));

    time_t ulTick = time(NULL);
    char szTime[AC_MSS_SIGN_TIME_LEN] = { 0 };
    as_strftime(szTime, AC_MSS_SIGN_TIME_LEN, "%Y%m%d%H%M%S", ulTick);
    cJSON_AddItemToObject(root, "msgtimestamp", cJSON_CreateString((char*)&szTime[0]));

    cJSON_AddItemToObject(root, "cameraId", cJSON_CreateString(strCameraID.c_str()));
    cJSON_AddItemToObject(root, "streamType", cJSON_CreateString(strStreamType.c_str()));
    cJSON_AddItemToObject(root, "urlType", cJSON_CreateString("1"));

    std::string strReqMSg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    /* sent the http request */
    if(AS_ERROR_CODE_OK != send_http_get_request(strReqMSg)) {
        return AS_ERROR_CODE_FAIL;
    }
    if(0 == m_strRespMsg.length()) {
        return AS_ERROR_CODE_FAIL;
    }

    /* 2.parse the response */
    root = cJSON_Parse(m_strRespMsg.c_str());
    if (NULL == root) {
        return AS_ERROR_CODE_FAIL;
    }

    cJSON *resultCode = cJSON_GetObjectItem(root, "resultCode");
    if(NULL == resultCode) {
        cJSON_Delete(root);
        return AS_ERROR_CODE_FAIL;
    }

    int nResultCode = atoi(resultCode->string);
    if(0 != strncmp(AC_MSS_ERROR_CODE_OK,resultCode->string,strlen(AC_MSS_ERROR_CODE_OK))) {
        cJSON_Delete(root);
        return AS_ERROR_CODE_FAIL;
    }

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if(NULL == url) {
        cJSON_Delete(root);
        return AS_ERROR_CODE_FAIL;
    }
    strRtspUrl = resultCode->string;
    cJSON_Delete(root);
    return AS_ERROR_CODE_OK;
}
void    ASEvLiveHttpClient::report_check_task_status(std::string& strUrl,ASRtspCheckTask* task)
{
    if (AS_ERROR_CODE_OK != open_http_by_url(strUrl)) {
        return ;
    }

    /* 1.build the request xml message */
    XMLDocument report;
    XMLPrinter printer;
    XMLDeclaration *declare = report.NewDeclaration();
    XMLElement *RepoetEl = report.NewElement("report");
    report.InsertEndChild(RepoetEl);
    RepoetEl->SetAttribute("version", "1.0");
    XMLElement *SesEle = report.NewElement("session");
    RepoetEl->InsertEndChild(SesEle);

    /*SesEle->SetAttribute("sessionid", strSessionID.c_str());
    if (SIP_SESSION_STATUS_ADD == enStatus)
    {
        SesEle->SetAttribute("status", "start");
    }
    else if ((SIP_SESSION_STATUS_REG == enStatus) || (SIP_SESSION_STATUS_RUNING == enStatus))
    {
        SesEle->SetAttribute("status", "running");
    }
    else if (SIP_SESSION_STATUS_REMOVE == enStatus)
    {
        SesEle->SetAttribute("status", "stop");
    }
    else
    {
        SesEle->SetAttribute("status", "stop");
    }*/

    report.Accept(&printer);
    std::string strRespMsg = printer.CStr();
    /* sent the http request */
    if (AS_ERROR_CODE_OK != send_http_get_request(strRespMsg)) {
        return ;
    }
    return;
}
void ASEvLiveHttpClient::handle_remote_read(struct evhttp_request* remote_rsp)
{
    if (NULL == remote_rsp){
        event_base_loopexit(m_pBase, NULL);
        return;
    }
    size_t len = evbuffer_get_length(remote_rsp->input_buffer);
    const char * str = (const char*)evbuffer_pullup(remote_rsp->input_buffer, len);
    if ((0 == len) || (NULL == str)) {
        m_strRespMsg = "";
        event_base_loopexit(m_pBase, NULL);
        return;
    }
    m_strRespMsg.append(str, 0, len);
    event_base_loopexit(m_pBase, NULL);
}

void ASEvLiveHttpClient::handle_readchunk(struct evhttp_request* remote_rsp)
{
    return;
}

void ASEvLiveHttpClient::handle_remote_connection_close(struct evhttp_connection* connection)
{
    event_base_loopexit(m_pBase, NULL);
}

void ASEvLiveHttpClient::remote_read_cb(struct evhttp_request* remote_rsp, void* arg)
{
    ASEvLiveHttpClient* client = (ASEvLiveHttpClient*)arg;
    client->handle_remote_read(remote_rsp);
    return;
}

void ASEvLiveHttpClient::readchunk_cb(struct evhttp_request* remote_rsp, void* arg)
{
    ASEvLiveHttpClient* client = (ASEvLiveHttpClient*)arg;
    client->handle_readchunk(remote_rsp);
    return;
}

void ASEvLiveHttpClient::remote_connection_close_cb(struct evhttp_connection* connection, void* arg)
{
    ASEvLiveHttpClient* client = (ASEvLiveHttpClient*)arg;
    client->handle_remote_connection_close(connection);
    return;
}

int32_t ASEvLiveHttpClient::open_http_by_url(std::string& strUrl)
{
    struct evhttp_uri* uri = evhttp_uri_parse(strUrl.c_str());
    if (!uri)
    {
        return AS_ERROR_CODE_FAIL;
    }

    int port = evhttp_uri_get_port(uri);
    if (port < 0) {
        port = AC_MSS_PORT_DAFAULT;
    }
    const char *host = evhttp_uri_get_host(uri);
    const char *path = evhttp_uri_get_path(uri);
    if (NULL == host)
    {
        evhttp_uri_free(uri);
        return AS_ERROR_CODE_FAIL;
    }
    if (path == NULL || strlen(path) == 0)
    {
        m_reqPath = "/";
    }
    else
    {
        m_reqPath = path;
    }



    m_pBase = event_base_new();
    if (!m_pBase)
    {
        evhttp_uri_free(uri);
        return AS_ERROR_CODE_FAIL;
    }

    m_pReq = evhttp_request_new(remote_read_cb, this);
    evhttp_request_set_chunked_cb(m_pReq, readchunk_cb);

    m_pConn = evhttp_connection_new( host, port);
    if (!m_pConn)
    {
        evhttp_uri_free(uri);
        return AS_ERROR_CODE_FAIL;
    }
    evhttp_connection_set_base(m_pConn, m_pBase);
    evhttp_connection_set_closecb(m_pConn, remote_connection_close_cb, this);
    evhttp_uri_free(uri);
    return AS_ERROR_CODE_OK;
}

int32_t ASEvLiveHttpClient::send_http_post_request(std::string& strMsg)
{
    struct evbuffer *buf = NULL;
    buf = evbuffer_new();
    if (NULL == buf)
    {
        return AS_ERROR_CODE_FAIL;
    }

    char lenbuf[33] = { 0 };
    snprintf(lenbuf, 32, "%lu", strMsg.length());
    evhttp_add_header(m_pReq->output_headers, "Content-Type", "text/plain; charset=UTF-8");
    evhttp_add_header(m_pReq->output_headers, "Content-length", lenbuf); //content length
    evhttp_add_header(m_pReq->output_headers, "Connection", "close");
    evbuffer_add_printf(buf, "%s", strMsg.c_str());
    evbuffer_add_buffer(m_pReq->output_buffer, buf);
    evhttp_make_request(m_pConn, m_pReq, EVHTTP_REQ_POST, m_reqPath.c_str());
    evhttp_connection_set_timeout(m_pReq->evcon, 600);
    event_base_dispatch(m_pBase);
    return AS_ERROR_CODE_OK;
}
int32_t ASEvLiveHttpClient::send_http_get_request(std::string& strMsg)
{
    struct evbuffer *buf = NULL;
    buf = evbuffer_new();
    if (NULL == buf)
    {
        return AS_ERROR_CODE_FAIL;
    }

    char lenbuf[33] = { 0 };
    snprintf(lenbuf, 32, "%lu", strMsg.length());
    evhttp_add_header(m_pReq->output_headers, "Content-Type", "text/plain; charset=UTF-8");
    evhttp_add_header(m_pReq->output_headers, "Content-length", lenbuf); //content length
    evhttp_add_header(m_pReq->output_headers, "Connection", "close");
    evbuffer_add_printf(buf, "%s", strMsg.c_str());
    evbuffer_add_buffer(m_pReq->output_buffer, buf);
    evhttp_make_request(m_pConn, m_pReq, EVHTTP_REQ_GET, m_reqPath.c_str());
    evhttp_connection_set_timeout(m_pReq->evcon, 600);
    event_base_dispatch(m_pBase);

    return AS_ERROR_CODE_OK;
}


ASRtspGuardManager::ASRtspGuardManager()
{
    m_ulTdIndex        = 0;
    m_LoopWatchVar     = 0;
    m_ulRecvBufSize    = RTSP_SOCKET_RECV_BUFFER_SIZE_DEFAULT;
    m_HttpThreadHandle = NULL;
    m_httpBase         = NULL;
    m_httpServer       = NULL;
    m_httpListenPort   = GW_SERVER_PORT_DEFAULT;
    m_mutex            = NULL;
    memset(m_ThreadHandle,0,sizeof(as_thread_t*)*RTSP_MANAGE_ENV_MAX_COUNT);
    memset(m_envArray,0,sizeof(UsageEnvironment*)*RTSP_MANAGE_ENV_MAX_COUNT);
    memset(m_clCountArray,0,sizeof(u_int32_t)*RTSP_MANAGE_ENV_MAX_COUNT);
    m_ulLogLM          = AS_LOG_WARNING;
    m_strAppID         = "";
    m_strAppSecret     = "";
    m_strAppKey        = "";
    m_strAppKey        = "";
    m_ulRtspHandlCount = 0;
}

ASRtspGuardManager::~ASRtspGuardManager()
{
}

int32_t ASRtspGuardManager::init()
{

    /* read the system config file */
    if (AS_ERROR_CODE_OK != read_system_conf()) {
        return AS_ERROR_CODE_FAIL;
    }

    /* start the log module */
    ASSetLogLevel(m_ulLogLM);
    ASSetLogFilePathName(RTSP2SIP_LOG_FILE);
    ASStartLog();


    m_mutex = as_create_mutex();
    if(NULL == m_mutex) {
        return AS_ERROR_CODE_FAIL;
    }

    return AS_ERROR_CODE_OK;
}
void    ASRtspGuardManager::release()
{

    m_LoopWatchVar = 1;
    as_destroy_mutex(m_mutex);
    m_mutex = NULL;
    ASStopLog();
}

int32_t ASRtspGuardManager::open()
{

    // Begin by setting up our usage environment:
    u_int32_t i = 0;



    m_LoopWatchVar = 0;
    /* start the http server deal thread */
    if (AS_ERROR_CODE_OK != as_create_thread((AS_THREAD_FUNC)http_env_invoke,
        this, &m_HttpThreadHandle, AS_DEFAULT_STACK_SIZE)) {
        return AS_ERROR_CODE_FAIL;
    }
    /* start the rtsp client deal thread */
    for(i = 0;i < RTSP_MANAGE_ENV_MAX_COUNT;i++) {
        m_envArray[i] = NULL;
        m_clCountArray[i] = 0;
    }

    for(i = 0;i < RTSP_MANAGE_ENV_MAX_COUNT;i++) {
        if( AS_ERROR_CODE_OK != as_create_thread((AS_THREAD_FUNC)rtsp_env_invoke,
                                                 this,&m_ThreadHandle[i],AS_DEFAULT_STACK_SIZE)) {
            return AS_ERROR_CODE_FAIL;
        }

    }

    return 0;
}

void ASRtspGuardManager::close()
{
    m_LoopWatchVar = 1;

    return;
}
AS_HANDLE ASRtspGuardManager::openURL(char const* rtspURL,ASRtspStatusObervser* observer)
{
    as_mutex_lock(m_mutex);
    TaskScheduler* scheduler = NULL;
    UsageEnvironment* env = NULL;
    u_int32_t index =  0;


    index = find_beast_thread();
    env = m_envArray[index];

    ASRtsp2RtpChannel* rtspClient = ASRtsp2RtpChannel::createNew(index,*env, rtspURL,
                                    RTSP_CLIENT_VERBOSITY_LEVEL, RTSP_AGENT_NAME);
    if (rtspClient == NULL) {
        as_mutex_unlock(m_mutex);
        return NULL;
    }

    m_clCountArray[index]++;
    m_ulRtspHandlCount++;

    ASRtsp2RtpChannel* AsRtspClient = (ASRtsp2RtpChannel*)rtspClient;
    AsRtspClient->open(RTSP_CLINET_RUN_DURATION,observer);
    as_mutex_unlock(m_mutex);
    return (AS_HANDLE)AsRtspClient;
}
void      ASRtspGuardManager::closeURL(AS_HANDLE handle)
{
    as_mutex_lock(m_mutex);
    TaskScheduler* scheduler = NULL;
    UsageEnvironment* env = NULL;
    ASRtsp2RtpChannel* pAsRtspClient = (ASRtsp2RtpChannel*)handle;

    u_int32_t index = pAsRtspClient->index();
    m_clCountArray[index]--;
    m_ulRtspHandlCount--;
    pAsRtspClient->close();
    as_mutex_unlock(m_mutex);
}


int32_t ASRtspGuardManager::read_system_conf()
{
    as_ini_config config;
    std::string   strValue="";
    if(INI_SUCCESS != config.ReadIniFile(RTSP2SIP_CONF_FILE))
    {
        return AS_ERROR_CODE_FAIL;
    }

    /* log level */
    if(INI_SUCCESS == config.GetValue("LOG_CFG","LogLM",strValue))
    {
        m_ulLogLM = atoi(strValue.c_str());
    }

    /* http listen port */
    if(INI_SUCCESS == config.GetValue("LISTEN_PORT","ListenPort",strValue))
    {
        m_httpListenPort = atoi(strValue.c_str());
    }

    /* ACS AppID */
    if(INI_SUCCESS == config.GetValue("ACS_CFG","AppID",strValue))
    {
        m_strAppID = strValue;
    }
    /* ACS AppSecret */
    if(INI_SUCCESS == config.GetValue("ACS_CFG","AppSecret",strValue))
    {
        m_strAppSecret = strValue;
    }
    /* ACS AppKey */
    if(INI_SUCCESS == config.GetValue("ACS_CFG","AppKey",strValue))
    {
        m_strAppKey = strValue;
    }
    /* ACS CallUrl */
    if(INI_SUCCESS == config.GetValue("ACS_CFG","CallUrl",strValue))
    {
        m_strLiveUrl = strValue;
    }
    return AS_ERROR_CODE_OK;
}

void  ASRtspGuardManager::http_callback(struct evhttp_request *req, void *arg)
{
    ASRtspGuardManager* pManage = (ASRtspGuardManager*)arg;
    pManage->handle_http_req(req);
}

void *ASRtspGuardManager::http_env_invoke(void *arg)
{
    ASRtspGuardManager* manager = (ASRtspGuardManager*)(void*)arg;
    manager->http_env_thread();
    return NULL;
}

void *ASRtspGuardManager::rtsp_env_invoke(void *arg)
{
    ASRtspGuardManager* manager = (ASRtspGuardManager*)(void*)arg;
    manager->rtsp_env_thread();
    return NULL;
}
void *ASRtspGuardManager::check_task_invoke(void *arg)
{
    ASRtspGuardManager* manager = (ASRtspGuardManager*)(void*)arg;
    manager->check_task_thread();
    return NULL;
}

void ASRtspGuardManager::http_env_thread()
{
    AS_LOG(AS_LOG_INFO,"ASRtspGuardManager::http_env_thread begin.");
    m_httpBase = event_base_new();
    if (NULL == m_httpBase)
    {
        AS_LOG(AS_LOG_CRITICAL,"ASRtspGuardManager::http_env_thread,create the event base fail.");
        return;
    }
    m_httpServer = evhttp_new(m_httpBase);
    if (NULL == m_httpServer)
    {
        AS_LOG(AS_LOG_CRITICAL,"ASRtspGuardManager::http_env_thread,create the http base fail.");
        return;
    }

    int ret = evhttp_bind_socket(m_httpServer, GW_SERVER_ADDR, m_httpListenPort);
    if (0 != ret)
    {
        AS_LOG(AS_LOG_CRITICAL,"ASRtspGuardManager::http_env_thread,bind the http socket fail.");
        return;
    }

    evhttp_set_timeout(m_httpServer, HTTP_OPTION_TIMEOUT);
    evhttp_set_gencb(m_httpServer, http_callback, this);
    event_base_dispatch(m_httpBase);

    AS_LOG(AS_LOG_INFO,"ASRtspGuardManager::http_env_thread end.");
    return;
}
void ASRtspGuardManager::rtsp_env_thread()
{
    u_int32_t index = thread_index();

    TaskScheduler* scheduler = NULL;
    UsageEnvironment* env = NULL;


    if(RTSP_MANAGE_ENV_MAX_COUNT <= index) {
        return;
    }
    scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);
    m_envArray[index] = env;
    m_clCountArray[index] = 0;


    // All subsequent activity takes place within the event loop:
    env->taskScheduler().doEventLoop(&m_LoopWatchVar);

    // LOOP EXIST
    env->reclaim();
    env = NULL;
    delete scheduler;
    scheduler = NULL;
    m_envArray[index] = NULL;
    m_clCountArray[index] = 0;
    return;
}

void ASRtspGuardManager::check_task_thread()
{
    while(0 == m_LoopWatchVar)
    {
        as_sleep(GW_TIMER_CHECK_TASK);
        check_task_status();
    }
}


u_int32_t ASRtspGuardManager::find_beast_thread()
{
    as_mutex_lock(m_mutex);
    u_int32_t index = 0;
    u_int32_t count = 0xFFFFFFFF;
    for(u_int32_t i = 0; i < RTSP_MANAGE_ENV_MAX_COUNT;i++) {
        if(count > m_clCountArray[i]) {
            index = i;
            count = m_clCountArray[i];
        }
    }
    as_mutex_unlock(m_mutex);
    return index;
}
UsageEnvironment* ASRtspGuardManager::get_env(u_int32_t index)
{
    UsageEnvironment* env = m_envArray[index];
    m_clCountArray[index]++;
    return env;
}
void ASRtspGuardManager::releas_env(u_int32_t index)
{
    if (0 == m_clCountArray[index])
    {
        return;
    }
    m_clCountArray[index]--;
}


void ASRtspGuardManager::handle_http_req(struct evhttp_request *req)
{
    AS_LOG(AS_LOG_DEBUG, "ASRtspGuardManager::handle_http_req begin");

    if (NULL == req)
    {
        return;
    }

    string uri_str = req->uri;
    string::size_type pos = uri_str.find_last_of(HTTP_SERVER_URI);

    if(pos == string::npos) {
         evhttp_send_error(req, 404, "service was not found!");
         return;
    }
    AS_LOG(AS_LOG_DEBUG, "ASRtspGuardManager::handle_http_req request path[%s].", uri_str.c_str());

    evbuffer *pbuffer = req->input_buffer;
    string post_str;
    int n = 0;
    char  szBuf[HTTP_REQUEST_MAX + 1] = { 0 };
    while ((n = evbuffer_remove(pbuffer, &szBuf, HTTP_REQUEST_MAX - 1)) > 0)
    {
        szBuf[n] = '\0';
        post_str.append(szBuf, n);
    }

    AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_http_req, msg[%s]", post_str.c_str());

    std::string strResp = "";
    if(AS_ERROR_CODE_OK != handle_check(post_str,strResp))
    {
        evhttp_send_error(req, 404, "call service fail!");
        return;
    }

    struct evbuffer* evbuf = evbuffer_new();
    if (NULL == evbuf)
    {
        return;
    }
    evbuffer_add_printf(evbuf, "%s", strResp.c_str());

    evhttp_send_reply(req, HTTP_OK, "OK", evbuf);
    evbuffer_free(evbuf);
    AS_LOG(AS_LOG_DEBUG, "ASRtspGuardManager::handle_http_req end");
}


int32_t ASRtspGuardManager::handle_check(std::string &strReqMsg,std::string &strRespMsg)
{
    std::string strCheckID  = "";

    int32_t ret = AS_ERROR_CODE_OK;

    AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check,msg:[%s].",strReqMsg.c_str());



    XMLDocument doc;
    XMLError xmlerr = doc.Parse(strReqMsg.c_str(),strReqMsg.length());
    if(XML_SUCCESS != xmlerr)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check,parse xml msg:[%s] fail.",strReqMsg.c_str());
        return AS_ERROR_CODE_FAIL;
    }

    XMLElement *req = doc.RootElement();
    if(NULL == req)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check,get xml req node fail.");
        return AS_ERROR_CODE_FAIL;
    }

    XMLElement *check = req->FirstChildElement("check");
    if(NULL == check)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check,get xml session node fail.");
        return AS_ERROR_CODE_FAIL;
    }




    const char* checkid = check->Attribute("checkid");
    if(NULL == checkid)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check,get xml check id fail.");
        return AS_ERROR_CODE_FAIL;
    }
    strCheckID = checkid;

    ret = handle_check_task(check);

    XMLDocument resp;
    XMLPrinter printer;
    XMLDeclaration *declare = resp.NewDeclaration();
    XMLElement *respEle = resp.NewElement("resp");
    resp.InsertEndChild(respEle);
    respEle->SetAttribute("version", "1.0");
    XMLElement *SesEle = resp.NewElement("check");
    respEle->InsertEndChild(SesEle);

    if(AS_ERROR_CODE_OK == ret)
    {
        respEle->SetAttribute("err_code","0");
        respEle->SetAttribute("err_msg","success");
    }
    else
    {
        respEle->SetAttribute("err_code","-1");
        respEle->SetAttribute("err_msg","fail");
    }

    SesEle->SetAttribute("checkid",checkid);

    resp.Accept(&printer);
    strRespMsg = printer.CStr();

    AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_session,end");
    return AS_ERROR_CODE_OK;
}
int32_t ASRtspGuardManager::handle_check_task(const XMLElement *check)
{
    bool bRegister            = false;
    std::string strReportURL  = "";
    std::string strCheckID    = "";
    std::string strCameraID   = "";
    std::string strStreamType = "";
    u_int32_t   ulInterval    = GW_REPORT_DEFAULT;
    ASRtspCheckTask* task     = NULL;


    const char* checkid = check->Attribute("checkid");
    if(NULL != checkid)
    {
        strCheckID = checkid;
    }

   const XMLElement *report = check->FirstChildElement("report");
    if(NULL == report)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::add_session,get xml report node fail.");
        return AS_ERROR_CODE_FAIL;
    }
    const char* interval = report->Attribute("interval");
    if(NULL != interval)
    {
        ulInterval = atoi(interval);
    }
    const char* url      = report->Attribute("url");
    if(NULL != url)
    {
        strReportURL = url;
    }

    const XMLElement *CameraList = check->FirstChildElement("cameralist");
    if(NULL == CameraList)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::add_session,get xml cameralist node fail.");
        return AS_ERROR_CODE_FAIL;
    }

    task = AS_NEW(task);
    if(NULL == task)
    {
        AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::add_session,create task fail.");
        return AS_ERROR_CODE_FAIL;
    }

    task->setTaskInfo(strCheckID,strReportURL);


    const XMLElement *camera = CameraList->FirstChildElement("camera");

    const char* cameraId  = NULL;
    const char* streamType = NULL;
    uint32_t count = 0;
    while(camera)
    {
        cameraId = camera->Attribute("ID");
        streamType = camera->Attribute("streamType");

        if(NULL == cameraId)
        {
            camera = camera->NextSiblingElement();
            continue;
        }
        strCameraID   = cameraId;
        strStreamType = streamType;

        task->addCamera(strCameraID,strStreamType);
        count++;

        camera = camera->NextSiblingElement();
    }


    AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::handle_check_task,CheckID:[%s],camera count:[%d].",
        strCheckID.c_str(), count);

    as_lock_guard locker(m_mutex);
    m_TaskList.push_back(task);

    AS_LOG(AS_LOG_INFO, "ASRtspGuardManager::add_session,end");
    return AS_ERROR_CODE_OK;
}

void  ASRtspGuardManager::check_task_status()
{
    ASRtspCheckTask* task     = NULL;
    as_lock_guard locker(m_mutex);
    ASCHECKTASKLISTITER iter = m_TaskList.begin();
    for(; iter != m_TaskList.end();)
    {
        task = *iter;
        task->checkTask();

        if(AS_RTSP_CHECK_STATUS_END == task->TaskStatus())
        {
            iter = m_TaskList.erase(iter);
            AS_DELETE(task);
        }
        else
        {
            ++iter;
        }
    }
}


void ASRtspGuardManager::setRecvBufSize(u_int32_t ulSize)
{
    m_ulRecvBufSize = ulSize;
}
u_int32_t ASRtspGuardManager::getRecvBufSize()
{
    return m_ulRecvBufSize;
}




