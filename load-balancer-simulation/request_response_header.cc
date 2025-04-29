#include "request_response_header.h"

#include "ns3/log.h"
#include "ns3/simulator.h" // For Simulator::Now() if used for default timestamp (not in this constructor)
#include "ns3/buffer.h"    // For Buffer::Iterator serialization/deserialization

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RequestResponseHeader");
NS_OBJECT_ENSURE_REGISTERED(RequestResponseHeader); // Ensure TypeId system registration

TypeId
RequestResponseHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RequestResponseHeader")
                            .SetParent<Header>()
                            .SetGroupName("Applications") // Or "Headers" if more appropriate
                            .AddConstructor<RequestResponseHeader>();
    return tid;
}

RequestResponseHeader::RequestResponseHeader()
    : m_seq(0),
      m_timestamp(Seconds(0.0)), // Initialize timestamp to zero
      m_payloadSize(0),
      m_l7Identifier(0)
{
    NS_LOG_FUNCTION(this);
}

RequestResponseHeader::~RequestResponseHeader()
{
    NS_LOG_FUNCTION(this);
}

TypeId
RequestResponseHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
RequestResponseHeader::Print(std::ostream& os) const
{
    // Print all member variables for debugging and logging
    os << "Seq=" << m_seq
       << ", Timestamp=" << m_timestamp.GetSeconds() << "s"
       << " (or " << m_timestamp.GetNanoSeconds() << "ns)" // Also show ns for precision
       << ", PayloadSize=" << m_payloadSize
       << ", L7Id=" << m_l7Identifier;
}

uint32_t
RequestResponseHeader::GetSerializedSize() const
{
    // Calculate the total size of the header when serialized:
    // Sequence Number (uint32_t)
    // Timestamp (int64_t, as nanoseconds)
    // Payload Size (uint32_t)
    // L7 Identifier (uint64_t)
    return sizeof(m_seq) + sizeof(int64_t) + sizeof(m_payloadSize) + sizeof(m_l7Identifier);
}

void
RequestResponseHeader::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);

    // Write members in network byte order (Host TO Network = hton)
    start.WriteHtonU32(m_seq);
    start.WriteHtonU64(m_timestamp.GetNanoSeconds()); // Serialize timestamp as nanoseconds
    start.WriteHtonU32(m_payloadSize);
    start.WriteHtonU64(m_l7Identifier);
}

uint32_t
RequestResponseHeader::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);

    // Read members in network byte order (Network TO Host = ntoh)
    m_seq = start.ReadNtohU32();
    int64_t timeNs = start.ReadNtohU64(); // Read timestamp as nanoseconds
    m_timestamp = NanoSeconds(timeNs);    // Convert back to ns3::Time
    m_payloadSize = start.ReadNtohU32();
    m_l7Identifier = start.ReadNtohU64();

    // Return the number of bytes read, which should match GetSerializedSize()
    return GetSerializedSize();
}

// --- Accessor and Mutator Implementations ---

void
RequestResponseHeader::SetSeq(uint32_t seq)
{
    m_seq = seq;
}

uint32_t
RequestResponseHeader::GetSeq() const
{
    return m_seq;
}

void
RequestResponseHeader::SetTimestamp(Time time)
{
    m_timestamp = time;
}

Time
RequestResponseHeader::GetTimestamp() const
{
    return m_timestamp;
}

void
RequestResponseHeader::SetPayloadSize(uint32_t size)
{
    m_payloadSize = size;
}

uint32_t
RequestResponseHeader::GetPayloadSize() const
{
    return m_payloadSize;
}

void
RequestResponseHeader::SetL7Identifier(uint64_t id)
{
    m_l7Identifier = id;
}

uint64_t
RequestResponseHeader::GetL7Identifier() const
{
    return m_l7Identifier;
}

} // namespace ns3
