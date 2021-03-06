/*
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301  USA
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "VNSISession.h"
#include "client.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "responsepacket.h"
#include "requestpacket.h"
#include "vnsicommand.h"
#include "tools.h"
#include "../../../lib/platform/sockets/tcp.h"
#include "../../../lib/platform/util/timeutils.h"

/* Needed on Mac OS/X */
 
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

using namespace ADDON;
using namespace PLATFORM;

cVNSISession::cVNSISession()
  : m_protocol(0)
  , m_socket(NULL)
  , m_connectionLost(false)
{
}

cVNSISession::~cVNSISession()
{
  Close();
}

void cVNSISession::Close()
{
  if(IsOpen())
  {
    m_socket->Close();
  }
  if (m_socket)
  {
    delete m_socket;
    m_socket = NULL;
  }
}

bool cVNSISession::Open(const std::string& hostname, int port, const char *name)
{
  Close();

  uint64_t iNow = GetTimeMs();
  uint64_t iTarget = iNow + g_iConnectTimeout * 1000;
  if (!m_socket)
    m_socket = new CTcpConnection(hostname.c_str(), port);
  while (!m_socket->IsOpen() && iNow < iTarget)
  {
    if (!m_socket->Open(iTarget - iNow))
      CEvent::Sleep(100);
    iNow = GetTimeMs();
  }

  if (!m_socket->IsOpen())
  {
    XBMC->Log(LOG_ERROR, "%s - failed to connect to the backend (%s)", __FUNCTION__, m_socket->GetError().c_str());
    return false;
  }

  // store connection data
  m_hostname = hostname;
  m_port = port;

  if(name != NULL)
    m_name = name;

  return true;
}

bool cVNSISession::Login()
{
  try
  {
    cRequestPacket vrp;
    if (!vrp.init(VNSI_LOGIN))                  throw "Can't init cRequestPacket";
    if (!vrp.add_U32(VNSI_PROTOCOLVERSION))     throw "Can't add protocol version to RequestPacket";
    if (!vrp.add_U8(false))                     throw "Can't add netlog flag";
    if (!m_name.empty())
    {
      if (!vrp.add_String(m_name.c_str()))      throw "Can't add client name to RequestPacket";
    }
    else
    {
      if (!vrp.add_String("XBMC Media Center")) throw "Can't add client name to RequestPacket";
    }

    // read welcome
    cResponsePacket* vresp = ReadResult(&vrp);
    if (!vresp)
      throw "failed to read greeting from server";

    uint32_t    protocol      = vresp->extract_U32();
    uint32_t    vdrTime       = vresp->extract_U32();
    int32_t     vdrTimeOffset = vresp->extract_S32();
    const char *ServerName    = vresp->extract_String();
    const char *ServerVersion = vresp->extract_String();

    m_server    = ServerName;
    m_version   = ServerVersion;
    m_protocol  = (int)protocol;

    if (m_protocol < VNSI_MIN_PROTOCOLVERSION)
      throw "Protocol versions do not match";

    if (m_name.empty())
      XBMC->Log(LOG_NOTICE, "Logged in at '%lu+%i' to '%s' Version: '%s' with protocol version '%d'",
        vdrTime, vdrTimeOffset, ServerName, ServerVersion, protocol);

    delete[] ServerName;
    delete[] ServerVersion;

    delete vresp;
  }
  catch (const char * str)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__,str);
    if (m_socket)
    {
      m_socket->Close();
      delete m_socket;
      m_socket = NULL;
    }
    return false;
  }

  return true;
}

cResponsePacket* cVNSISession::ReadMessage(int iInitialTimeout /*= 10000*/, int iDatapacketTimeout /*= 10000*/)
{
  uint32_t channelID = 0;
  uint32_t userDataLength = 0;
  uint8_t* userData = NULL;

  cResponsePacket* vresp = NULL;

  CLockObject lock(m_readMutex);

  if(!readData((uint8_t*)&channelID, sizeof(uint32_t), iInitialTimeout))
    return NULL;

  // Data was read

  channelID = ntohl(channelID);
  if (channelID == VNSI_CHANNEL_STREAM)
  {
    vresp = new cResponsePacket();

    if (!readData(vresp->getHeader(), vresp->getStreamHeaderLength(), iDatapacketTimeout))
    {
      delete vresp;
      XBMC->Log(LOG_ERROR, "%s - lost sync on channel stream packet", __FUNCTION__);
      SignalConnectionLost();
      return NULL;
    }
    vresp->extractStreamHeader();
    userDataLength = vresp->getUserDataLength();

    if(vresp->getOpCodeID() == VNSI_STREAM_MUXPKT)
    {
      DemuxPacket* p = PVR->AllocateDemuxPacket(userDataLength);
      userData = (uint8_t*)p;
      if (userDataLength > 0)
      {
        if (!userData) return NULL;
        if (!readData(p->pData, userDataLength, iDatapacketTimeout))
        {
          PVR->FreeDemuxPacket(p);
          delete vresp;
          XBMC->Log(LOG_ERROR, "%s - lost sync on channel stream mux packet", __FUNCTION__);
          SignalConnectionLost();
          return NULL;
        }
      }
    }
    else if (userDataLength > 0)
    {
      userData = (uint8_t*)malloc(userDataLength);
      if (!userData) return NULL;
      if (!readData(userData, userDataLength, iDatapacketTimeout))
      {
        free(userData);
        delete vresp;
        XBMC->Log(LOG_ERROR, "%s - lost sync on channel stream (other) packet", __FUNCTION__);
        SignalConnectionLost();
        return NULL;
      }
    }
    vresp->setStream(userData, userDataLength);
  }
  else if (channelID == VNSI_CHANNEL_OSD)
  {
    vresp = new cResponsePacket();

    if (!readData(vresp->getHeader(), vresp->getOSDHeaderLength(), iDatapacketTimeout))
    {
      XBMC->Log(LOG_ERROR, "%s - lost sync on osd packet", __FUNCTION__);
      SignalConnectionLost();
      return NULL;
    }
    vresp->extractOSDHeader();
    userDataLength = vresp->getUserDataLength();

    if (userDataLength > 5000000) return NULL; // how big can these packets get?
    userData = NULL;
    if (userDataLength > 0)
    {
      userData = (uint8_t*)malloc(userDataLength);
      if (!userData) return NULL;
      if (!readData(userData, userDataLength, iDatapacketTimeout))
      {
        free(userData);
        delete vresp;
        XBMC->Log(LOG_ERROR, "%s - lost sync on additional osd packet", __FUNCTION__);
        SignalConnectionLost();
        return NULL;
      }
    }
    vresp->setOSD(userData, userDataLength);
  }
  else
  {
    vresp = new cResponsePacket();

    if (!readData(vresp->getHeader(), vresp->getHeaderLength(), iDatapacketTimeout))
    {
      delete vresp;
      XBMC->Log(LOG_ERROR, "%s - lost sync on response packet", __FUNCTION__);
      SignalConnectionLost();
      return NULL;
    }
    vresp->extractHeader();
    userDataLength = vresp->getUserDataLength();

    if (userDataLength > 5000000) return NULL; // how big can these packets get?
    userData = NULL;
    if (userDataLength > 0)
    {
      userData = (uint8_t*)malloc(userDataLength);
      if (!userData) return NULL;
      if (!readData(userData, userDataLength, iDatapacketTimeout))
      {
        free(userData);
        delete vresp;
        XBMC->Log(LOG_ERROR, "%s - lost sync on additional response packet", __FUNCTION__);
        SignalConnectionLost();
        return NULL;
      }
    }

    if (channelID == VNSI_CHANNEL_STATUS)
      vresp->setStatus(userData, userDataLength);
    else
      vresp->setResponse(userData, userDataLength);
  }

  return vresp;
}

bool cVNSISession::TransmitMessage(cRequestPacket* vrp)
{
  if (!IsOpen())
    return false;

  ssize_t iWriteResult = m_socket->Write(vrp->getPtr(), vrp->getLen());
  if (iWriteResult != (ssize_t)vrp->getLen())
  {
    XBMC->Log(LOG_ERROR, "%s - Failed to write packet (%s), bytes written: %d of total: %d", __FUNCTION__, m_socket->GetError().c_str(), iWriteResult, vrp->getLen());
    return false;
  }
  return true;
}

cResponsePacket* cVNSISession::ReadResult(cRequestPacket* vrp)
{
  if(!TransmitMessage(vrp))
  {
    SignalConnectionLost();
    return NULL;
  }

  cResponsePacket *pkt = NULL;

  while((pkt = ReadMessage()))
  {
    /* Discard everything other as response packets until it is received */
    if (pkt->getChannelID() == VNSI_CHANNEL_REQUEST_RESPONSE && pkt->getRequestID() == vrp->getSerial())
    {
      return pkt;
    }
    else
      delete pkt;
  }

  SignalConnectionLost();
  return NULL;
}

bool cVNSISession::ReadSuccess(cRequestPacket* vrp)
{
  cResponsePacket *pkt = NULL;
  if((pkt = ReadResult(vrp)) == NULL)
  {
    return false;
  }
  uint32_t retCode = pkt->extract_U32();
  delete pkt;

  if(retCode != VNSI_RET_OK)
  {
    XBMC->Log(LOG_ERROR, "%s - failed with error code '%i'", __FUNCTION__, retCode);
    return false;
  }
  return true;
}

void cVNSISession::OnReconnect() {
}

void cVNSISession::OnDisconnect() {
}

bool cVNSISession::TryReconnect() {
  if(!Open(m_hostname, m_port))
    return false;

  if(!Login())
    return false;

  XBMC->Log(LOG_DEBUG, "%s - reconnected", __FUNCTION__);
  m_connectionLost = false;

  OnReconnect();

  return true;
}

bool cVNSISession::IsOpen()
{
  bool bReturn(false);
  if (m_socket && m_socket->IsOpen())
    bReturn = true;
  return bReturn;
}

void cVNSISession::SignalConnectionLost()
{
  if(m_connectionLost)
    return;

  XBMC->Log(LOG_ERROR, "%s - connection lost !!!", __FUNCTION__);

  m_connectionLost = true;
  Close();

  OnDisconnect();
}

bool cVNSISession::readData(uint8_t* buffer, int totalBytes, int timeout)
{
  int bytesRead = m_socket->Read(buffer, totalBytes, timeout);
  if (bytesRead == totalBytes)
    return true;
  else if (m_socket->GetErrorNumber() == ETIMEDOUT && bytesRead > 0)
  {
    // we did read something. try to finish the read
    bytesRead += m_socket->Read(buffer+bytesRead, totalBytes-bytesRead, timeout);
    if (bytesRead == totalBytes)
      return true;
  }
  else if (m_socket->GetErrorNumber() == ETIMEDOUT)
  {
    return false;
  }

  SignalConnectionLost();
  return false;
}

void cVNSISession::SleepMs(int ms)
{
  CEvent::Sleep(ms);
}
