#ifndef RING_HASH_LOAD_BALANCER_H
#define RING_HASH_LOAD_BALANCER_H

// Base class
#include "load_balancer.h"

// NS-3 Includes
#include "ns3/nstime.h" // For Time (used in overridden virtual function signatures)

// Standard Library Includes
#include <vector>
#include <map>
#include <string>     // For std::string (used by std::hash<std::string>)
#include <functional> // For std::hash
#include <cstdint>    // For uint32_t, uint64_t
#include <utility>    // For std::pair (used in base class method signatures)

namespace ns3 {

// Forward declarations (if base class doesn't cover them for overridden methods)
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief Implements Ring Hash (often referred to as Consistent Hashing or Ketama-style)
 * load balancing.
 *
 * This load balancer distributes requests to backend servers by mapping both servers
 * and request keys (derived from an L7 identifier) onto a circular hash space (the "ring").
 * Each backend server is represented by multiple virtual nodes on this ring,
 * proportional to its weight, to improve distribution.
 * A request's key is hashed, and the request is assigned to the server whose
 * virtual node is first encountered clockwise on the ring from the request's hash point.
 * This method aims for even distribution and minimizes remappings when servers are
 * added or removed.
 */
class RingHashLoadBalancer : public LoadBalancerApp
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    RingHashLoadBalancer();
    virtual ~RingHashLoadBalancer() override;

    // Override backend management methods to trigger ring recalculation
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress) override; // Adds with default weight

  protected:
    /**
     * @brief Chooses a backend using the hash ring.
     * The L7 identifier from the request is hashed to find a point on the ring,
     * and the request is assigned to the backend server that "owns" that point
     * (i.e., the next server encountered clockwise on the ring).
     *
     * @param packet The incoming request packet (content not used by this algorithm).
     * @param fromAddress The source address of the request (not used by this algorithm).
     * @param l7Identifier The Layer 7 identifier from the request, used for hashing.
     * @param[out] chosenBackend The InetSocketAddress of the selected backend server.
     * @return True if a backend was successfully chosen, false otherwise (e.g., ring not built or empty).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    /**
     * @brief Records latency for a backend. No-op for RingHashLoadBalancer.
     * Ring Hash selection is typically based on consistent hashing, not active latency.
     * @param backendAddress The address of the backend.
     * @param rtt The recorded round-trip time.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;

    /**
     * @brief Notification that a request has been sent to a backend. No-op for RingHashLoadBalancer.
     * Ring Hash does not use active request counts for selection.
     * @param backendAddress The address of the backend the request was sent to.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;

    /**
     * @brief Notification that a request to a backend has finished. No-op for RingHashLoadBalancer.
     * Ring Hash does not use active request counts for selection.
     * @param backendAddress The address of the backend whose request finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

  private:
    /**
     * @brief Recalculates the hash ring based on the current list of backend servers and their weights.
     * This method populates `m_ring` with hash points mapped to backend addresses.
     */
    void RecalculateRing();

    // --- Ring Hash Specific Member Variables ---
    uint64_t m_minRingSize; //!< Configurable minimum number of entries (virtual nodes) in the hash ring (attribute).
    uint64_t m_maxRingSize; //!< Configurable maximum number of entries (virtual nodes) in the hash ring (attribute).

    // Default values for ring size, inspired by common practices (e.g., Envoy defaults).
    static const uint64_t DefaultMinRingSize;     //!< Default minimum size of the hash ring.
    static const uint64_t DefaultMaxRingSize;     //!< Default maximum size of the hash ring.
    static const uint32_t DefaultHashesPerHost;   //!< Default number of virtual nodes to generate per host, as a baseline before weighting.

    //! The hash ring, implemented as a map where the key is the hash value (point on the ring)
    //! and the value is the InetSocketAddress of the backend server owning that point.
    std::map<uint64_t, InetSocketAddress> m_ring;

    std::hash<std::string> m_hasher; //!< Standard string hasher used for generating ring points and request hashes.
};

} // namespace ns3
#endif // RING_HASH_LOAD_BALANCER_H
