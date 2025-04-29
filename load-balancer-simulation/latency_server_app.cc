#include "latency_server_app.h"

#include "request_response_header.h" // Custom header
#include "ns3/log.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/core-module.h"    // For Ptr, ObjectFactory, TypeId, Callbacks, App basics
#include "ns3/buffer.h"

#include <algorithm> // For std::find
#include <string>
#include <vector>
#include <map>
#include <list>
#include <cstdint>
#include <sstream> // Required for std::ostringstream

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("LatencyServerApp");
// NS_OBJECT_ENSURE_REGISTERED is in .h

TypeId
LatencyServerApp::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::LatencyServerApp")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<LatencyServerApp>()
            .AddAttribute("Port",
                          "Port on which the server listens for connections.",
                          UintegerValue(9), 
                          MakeUintegerAccessor(&LatencyServerApp::m_port),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("ProcessingDelay",
                          "Simulated processing delay per request.",
                          TimeValue(MilliSeconds(0)), 
                          MakeTimeAccessor(&LatencyServerApp::m_processingDelay),
                          MakeTimeChecker());
    return tid;
}

LatencyServerApp::LatencyServerApp()
    : m_port(0), 
      m_listeningSocket(nullptr),
      m_processingDelay(MilliSeconds(0))
{
    NS_LOG_FUNCTION(this);
}

LatencyServerApp::~LatencyServerApp()
{
    NS_LOG_FUNCTION(this);
    m_listeningSocket = nullptr;
}

void
LatencyServerApp::SetProcessingDelay(Time delay)
{
    NS_LOG_FUNCTION(this << delay);
    m_processingDelay = delay;
}

uint64_t
LatencyServerApp::GetTotalRequestsReceived() const
{
    return m_requestsReceived;
}

void
LatencyServerApp::DoDispose()
{
    NS_LOG_FUNCTION(this);
    if (m_listeningSocket) {
        m_listeningSocket->Close();
        m_listeningSocket = nullptr;
    }
    for (Ptr<Socket> socket : m_socketList)
    {
        if (socket) {
            socket->Close();
        }
    }
    m_socketList.clear();
    m_rxBuffers.clear();
    Application::DoDispose();
}

void
LatencyServerApp::StartApplication()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s LatencyServerApp on Node " << GetNode()->GetId() << " starting.");

    if (!m_listeningSocket)
    {
        m_listeningSocket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        if (!m_listeningSocket)
        {
            NS_FATAL_ERROR("Node " << GetNode()->GetId() << ": Failed to create listening socket.");
            return; 
        }

        InetSocketAddress localAddress(Ipv4Address::GetAny(), m_port);
        if (m_listeningSocket->Bind(localAddress) != 0)
        {
            NS_FATAL_ERROR("Node " << GetNode()->GetId() << ": Failed to bind server socket to " << localAddress 
                           << ". Errno: " << m_listeningSocket->GetErrno());
        }
        if (m_listeningSocket->Listen() != 0)
        {
            NS_FATAL_ERROR("Node " << GetNode()->GetId() << ": Failed to listen on server socket " << localAddress
                           << ". Errno: " << m_listeningSocket->GetErrno());
        }
        
        m_listeningSocket->SetAcceptCallback(
            MakeNullCallback<bool, Ptr<Socket>, const Address&>(), 
            MakeCallback(&LatencyServerApp::HandleAccept, this));

        NS_LOG_INFO("Server (Node " << GetNode()->GetId() << ") listening on " << localAddress);
    }
}

void
LatencyServerApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s LatencyServerApp on Node " << GetNode()->GetId() << " stopping.");

    if (m_listeningSocket)
    {
        m_listeningSocket->Close();
        m_listeningSocket->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                             MakeNullCallback<void, Ptr<Socket>, const Address&>());
        m_listeningSocket = nullptr; 
    }

    for (Ptr<Socket> socket : m_socketList)
    {
        if (socket) {
            socket->Close();
        }
    }
    m_socketList.clear();
    m_rxBuffers.clear();
}

void
LatencyServerApp::HandleAccept(Ptr<Socket> newSocket, const Address& from)
{
    NS_LOG_FUNCTION(this << newSocket << from);
    InetSocketAddress inetFrom = InetSocketAddress::ConvertFrom(from);
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Server (Node " << GetNode()->GetId() 
                  << ") accepted connection from " << inetFrom);

    m_socketList.push_back(newSocket);
    m_rxBuffers.emplace(newSocket, ""); 

    newSocket->SetCloseCallbacks(MakeCallback(&LatencyServerApp::HandleClientClose, this),
                                 MakeCallback(&LatencyServerApp::HandleClientError, this));
    newSocket->SetRecvCallback(MakeCallback(&LatencyServerApp::HandleRead, this));
}

void
LatencyServerApp::HandleClientClose(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address from;
    std::string peerId = "unknown peer";
    if (socket->GetPeerName(from)) { 
        std::ostringstream oss;
        oss << InetSocketAddress::ConvertFrom(from);
        peerId = oss.str();
    } else {
        std::ostringstream oss;
        oss << "socket Ptr@" << static_cast<void*>(PeekPointer(socket));
        peerId = oss.str();
    }
    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Client " << peerId << " closed connection normally on Node " << GetNode()->GetId());
    
    m_rxBuffers.erase(socket);
    m_socketList.remove(socket);
}

void
LatencyServerApp::HandleClientError(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address from;
    std::string peerId = "unknown peer";
    Socket::SocketErrno err = socket->GetErrno(); 

    if (socket->GetPeerName(from)) {
        std::ostringstream oss;
        oss << InetSocketAddress::ConvertFrom(from);
        peerId = oss.str();
    } else {
        std::ostringstream oss;
        oss << "socket Ptr@" << static_cast<void*>(PeekPointer(socket));
        peerId = oss.str();
    }
    NS_LOG_WARN(Simulator::Now().GetSeconds() << "s Error on client socket " << peerId 
                  << " on Node " << GetNode()->GetId() << ". Errno: " << err);
    
    m_rxBuffers.erase(socket);
    m_socketList.remove(socket);
}


void
LatencyServerApp::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Ptr<Packet> packet;
    Address from; 
    const uint32_t headerSize = RequestResponseHeader().GetSerializedSize();

    auto bufferIt = m_rxBuffers.find(socket);
    if (bufferIt == m_rxBuffers.end()) {
        NS_LOG_ERROR("Server (Node " << GetNode()->GetId() << "): HandleRead called for unknown or closed socket " 
                       << static_cast<void*>(PeekPointer(socket)));
        return;
    }
    std::string& currentRxBuffer = bufferIt->second;

    while ((packet = socket->RecvFrom(from))) 
    {
        if (packet->GetSize() == 0) 
        {
            NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Server (Node " << GetNode()->GetId() 
                          << "): Peer " << InetSocketAddress::ConvertFrom(from) << " initiated graceful close.");
            break;
        }
        
        std::string received_chunk(packet->GetSize(), '\0');
        packet->CopyData(reinterpret_cast<uint8_t*>(received_chunk.data()), packet->GetSize());
        currentRxBuffer.append(received_chunk);

        NS_LOG_DEBUG("Server (Node " << GetNode()->GetId() << ") HandleRead: Received " << packet->GetSize() 
                       << " bytes from " << InetSocketAddress::ConvertFrom(from) 
                       << ". Buffer size for this socket: " << currentRxBuffer.size());

        while (currentRxBuffer.size() >= headerSize)
        {
            Ptr<Packet> headerPeekPacket = Create<Packet>(
                reinterpret_cast<const uint8_t*>(currentRxBuffer.data()),
                headerSize);

            RequestResponseHeader reqHeader;
            if (headerPeekPacket->PeekHeader(reqHeader) != headerSize) {
                 NS_LOG_WARN("Server (Node " << GetNode()->GetId() 
                               << "): Could not peek complete header from buffer. Data may be corrupt or logic error.");
                 break; 
            }

            uint32_t expectedPayloadSize = reqHeader.GetPayloadSize();
            uint32_t expectedTotalSize = headerSize + expectedPayloadSize;

            if (currentRxBuffer.size() >= expectedTotalSize) {
                NS_LOG_DEBUG("Server (Node " << GetNode()->GetId() 
                               << ") HandleRead: Processing complete request. Seq=" << reqHeader.GetSeq());
                ProcessRequest(socket, reqHeader, expectedPayloadSize);

                currentRxBuffer.erase(0, expectedTotalSize);
                NS_LOG_DEBUG("Server (Node " << GetNode()->GetId() << ") HandleRead: Consumed " 
                               << expectedTotalSize << " bytes. Buffer remaining: " << currentRxBuffer.size());
            }
            else 
            {
                NS_LOG_DEBUG("Server (Node " << GetNode()->GetId() 
                               << ") HandleRead: Incomplete request. Need " << expectedTotalSize 
                               << ", have " << currentRxBuffer.size() << ". Waiting for more data.");
                break; 
            }
        } 
    } 

    Socket::SocketErrno sockErr = socket->GetErrno();
    if (sockErr != Socket::ERROR_NOTERROR && sockErr != Socket::ERROR_AGAIN &&
        sockErr != Socket::ERROR_SHUTDOWN && sockErr != Socket::ERROR_NOTCONN)
    {
        NS_LOG_WARN("Server (Node " << GetNode()->GetId() 
                      << ") HandleRead: Error after RecvFrom loop for socket " 
                      << static_cast<void*>(PeekPointer(socket)) << ". Errno: " << sockErr);
    }
}

void
LatencyServerApp::ProcessRequest(Ptr<Socket> socket, RequestResponseHeader header, uint32_t payloadSize)
{
    NS_LOG_FUNCTION(this << socket << header.GetSeq() << payloadSize);
    m_requestsReceived++;

    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Server (Node " << GetNode()->GetId() 
                  << ") received request Seq=" << header.GetSeq()
                  << ", L7Id=" << header.GetL7Identifier() 
                  << ", PayloadSize=" << payloadSize 
                  << " (Total Server Rx: " << m_requestsReceived << ")");

    if (m_processingDelay > Time(0))
    {
        NS_LOG_DEBUG("Server (Node " << GetNode()->GetId() << "): Scheduling response for Seq=" 
                       << header.GetSeq() << " after delay " << m_processingDelay);
        Simulator::Schedule(m_processingDelay, &LatencyServerApp::SendResponse, this, socket, header);
    }
    else
    {
        SendResponse(socket, header);
    }
}

void
LatencyServerApp::SendResponse(Ptr<Socket> socket, RequestResponseHeader header)
{
    NS_LOG_FUNCTION(this << socket << header.GetSeq());

    if (std::find(m_socketList.begin(), m_socketList.end(), socket) == m_socketList.end()) {
        NS_LOG_WARN("Server (Node " << GetNode()->GetId() << "): Cannot send response to Seq=" 
                      << header.GetSeq() << ", socket is no longer valid or active.");
        return;
    }
    header.SetPayloadSize(0); 

    Ptr<Packet> responsePacket = Create<Packet>(0); 
    responsePacket->AddHeader(header);

    NS_LOG_INFO(Simulator::Now().GetSeconds() << "s Server (Node " << GetNode()->GetId() 
                  << ") sending response Seq=" << header.GetSeq() 
                  << ", L7Id=" << header.GetL7Identifier());

    int bytesSent = socket->Send(responsePacket);
    if (bytesSent < 0)
    {
        NS_LOG_WARN("Server (Node " << GetNode()->GetId() << "): Error sending response for Seq=" 
                      << header.GetSeq() << ". Errno: " << socket->GetErrno());
    } else if (static_cast<uint32_t>(bytesSent) < responsePacket->GetSize()) {
         NS_LOG_WARN("Server (Node " << GetNode()->GetId() << "): Could not send full response for Seq=" 
                       << header.GetSeq() << ". Sent " << bytesSent << "/" << responsePacket->GetSize()
                       << ". TCP will manage.");
    }
}

} // namespace ns3
