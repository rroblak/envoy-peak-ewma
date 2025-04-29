#ifndef REQUEST_RESPONSE_HEADER_H
#define REQUEST_RESPONSE_HEADER_H

// NS-3 Includes
#include "ns3/header.h" // Base class
#include "ns3/nstime.h" // For ns3::Time

// Standard Library Includes
#include <cstdint> // For uint32_t, uint64_t
#include <ostream> // For std::ostream (used in Print method)

namespace ns3 {

/**
 * @brief A custom header for request and response messages in network simulations.
 *
 * This header facilitates tracking and routing of messages by including:
 * - A sequence number (`m_seq`) for identifying individual messages or ordering.
 * - A timestamp (`m_timestamp`) typically used to mark the send time for latency calculations.
 * - The size of the payload (`m_payloadSize`) that follows this header in a packet.
 * - A Layer 7 identifier (`m_l7Identifier`) which can be used for consistent hashing
 * or flow identification by load balancers or other application-level entities.
 */
class RequestResponseHeader : public Header
{
  public:
    /**
     * @brief Gets the TypeId for this header class.
     * This is used by the ns-3 type system for object creation and introspection.
     * @return The TypeId associated with RequestResponseHeader.
     */
    static TypeId GetTypeId();

    RequestResponseHeader();
    virtual ~RequestResponseHeader() override;

    // --- ns3::Header virtual methods ---

    /**
     * @brief Returns the TypeId of this specific instance of the header.
     * @return The TypeId of RequestResponseHeader.
     */
    virtual TypeId GetInstanceTypeId() const override;

    /**
     * @brief Prints the contents of the header to the provided output stream.
     * Useful for debugging and logging.
     * @param os The output stream to print to.
     */
    virtual void Print(std::ostream& os) const override;

    /**
     * @brief Calculates and returns the serialized size of this header in bytes.
     * This is the number of bytes the header will occupy when written to a buffer.
     * @return The serialized size of the header.
     */
    virtual uint32_t GetSerializedSize() const override;

    /**
     * @brief Serializes the header's data members into a byte buffer.
     * Data is written in network byte order (big-endian).
     * @param start An iterator pointing to the start of the buffer segment where
     * the header should be written.
     */
    virtual void Serialize(Buffer::Iterator start) const override;

    /**
     * @brief Deserializes the header's data members from a byte buffer.
     * Data is read assuming network byte order.
     * @param start An iterator pointing to the start of the buffer segment from
     * which the header should be read.
     * @return The number of bytes read from the buffer (should match GetSerializedSize()).
     */
    virtual uint32_t Deserialize(Buffer::Iterator start) override;

    // --- Custom accessor and mutator methods ---

    /**
     * @brief Sets the sequence number for this header.
     * @param seq The sequence number.
     */
    void SetSeq(uint32_t seq);

    /**
     * @brief Gets the sequence number from this header.
     * @return The sequence number.
     */
    uint32_t GetSeq() const;

    /**
     * @brief Sets the timestamp for this header.
     * @param time The timestamp, typically representing a send or receive time.
     */
    void SetTimestamp(Time time);

    /**
     * @brief Gets the timestamp from this header.
     * @return The timestamp.
     */
    Time GetTimestamp() const;

    /**
     * @brief Sets the size of the payload that follows this header.
     * @param size The payload size in bytes.
     */
    void SetPayloadSize(uint32_t size);

    /**
     * @brief Gets the size of the payload that follows this header.
     * @return The payload size in bytes.
     */
    uint32_t GetPayloadSize() const;

    /**
     * @brief Sets the Layer 7 identifier for this header.
     * This identifier can be used for purposes like consistent hashing by load balancers.
     * @param id The L7 identifier.
     */
    void SetL7Identifier(uint64_t id);

    /**
     * @brief Gets the Layer 7 identifier from this header.
     * @return The L7 identifier.
     */
    uint64_t GetL7Identifier() const;

  private:
    uint32_t m_seq;          //!< Sequence number of the message.
    Time m_timestamp;        //!< Timestamp, e.g., for latency calculation.
    uint32_t m_payloadSize;  //!< Size of the payload immediately following this header.
    uint64_t m_l7Identifier; //!< Layer 7 identifier, e.g., for consistent hashing or flow tracking.
};

} // namespace ns3

#endif // REQUEST_RESPONSE_HEADER_H
