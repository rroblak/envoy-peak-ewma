# ns-3 Load Balancer Simulation

## Overview

This project uses the ns-3 network simulator to model and evaluate the performance of various Layer 7 (L7) load balancing algorithms. The primary goal is to compare the latency characteristics of [Envoy's existing load balancing strategies](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/load_balancing/load_balancers)—Weighted Round Robin (WRR), Least Request (LR), Random, Ring Hash, and Maglev—against the Peak Exponentially Weighted Moving Average (PeakEWMA) algorithm.

The simulation environment consists of multiple client nodes generating request-response traffic towards a central load balancer. This load balancer distributes the requests to a pool of backend server nodes, which can be configured with different processing delays to mimic real-world heterogeneous environments. The key performance metric is end-to-end request latency as observed by the clients.

## Key Findings: PeakEWMA Performance

The PeakEWMA algorithm demonstrates significantly improved latency performance, especially in scenarios with backend servers exhibiting heterogeneous response times. In a test scenario with one out of ten servers having a substantially higher processing delay (50ms vs. 5ms for others), PeakEWMA maintained lower average and tail latencies compared to other common algorithms.

**Test Scenario Parameters:**
* Number of Clients: 10
* Number of Servers: 10
* Server Weights: All servers have a weight of 1
* Server Delays: 9 servers are configured with a 5ms processing delay, and 1 server has a 50ms processing delay
* Client Requests per Client: 100
* Client Request Interval: 0.1 seconds
* Client Request Size: 100 bytes
* Total Simulation Time: 15 seconds

**Latency Results (ms):**

| Algorithm    | Min (ms)   | Avg (ms)   | P50 (ms) | P75 (ms)     | P90 (ms)   | P95 (ms)   | P99 (ms)   | Max (ms)    | Std Dev (ms) |
| :----------- | :--------- | :--------- | :--------- | :--------- | :--------- | :--------- | :--------- | :---------- | :----------- |
| WRR          | 5.0000     | 9.5920     | 5.0000     | 5.0000     | 24.0000    | 50.0000    | 50.0000    | 59.0000     | 13.5393      |
| LR           | 5.0000     | 8.9240     | 5.0000     | 5.0000     | 5.0000     | 50.0000    | 50.0000    | 63.0000     | 12.6056      |
| Random       | 5.0000     | 9.6140     | 5.0000     | 5.0000     | 23.0000    | 50.0000    | 50.0000    | 55.0000     | 13.5226      |
| RingHash     | 5.0000     | 9.3700     | 5.0000     | 5.0000     | 13.0000    | 50.0000    | 50.0000    | 63.0000     | 13.2148      |
| Maglev       | 5.0000     | 8.7130     | 5.0000     | 5.0000     | 5.0000     | 50.0000    | 50.0000    | 60.0000     | 12.2602      |
| **PeakEWMA** | **5.0000** | **5.1450** | **5.0000** | **5.0000** | **5.0000** | **5.0000** | **7.0000** | **63.0000** | **2.0591**   |

As shown, PeakEWMA significantly reduces average latency and provides much tighter tail latency distributions (P90-P99) and a lower standard deviation, indicating more consistent performance by effectively routing traffic away from the slower backend.

## Simulation Details

* **Topology:** The network consists of client nodes, a load balancer node, and backend server nodes connected via two CSMA (Carrier-Sense Multiple Access) LAN segments. Clients connect to the load balancer over the frontend LAN, and the load balancer connects to the servers over the backend LAN.

    ```mermaid
    graph LR
        subgraph Clients
            direction LR
            C1["Client 1<br>192.168.1.2"]
            Cdots[...]
            Cn["Client N<br>192.168.1.(N+1)"]
        end

        subgraph Frontend_LAN ["Frontend LAN (e.g., 192.168.1.0/24)"]
            %% Style the subgraph box
            style Frontend_LAN fill:#f9f,stroke:#333,stroke-width:1px,color:#333
            %% Node representing the CSMA bus
            F_Bus((LAN Bus))
            %% Style the bus node
            style F_Bus fill:#fff,stroke:#333,stroke-width:2px
        end

        %% Central Load Balancer Node
        LB["Load Balancer Node<br>VIP: 192.168.1.1<br>Internal: 10.10.0.1"]
        style LB fill:#ccf,stroke:#333,stroke-width:2px

        subgraph Backend_LAN ["Backend LAN (e.g., 10.10.0.0/24)"]
            %% Style the subgraph box
            style Backend_LAN fill:#f9f,stroke:#333,stroke-width:1px,color:#333
            %% Node representing the CSMA bus
            B_Bus((LAN Bus))
            %% Style the bus node
            style B_Bus fill:#fff,stroke:#333,stroke-width:2px
        end

        subgraph Servers
            direction LR
            S1["Server 1<br>10.10.0.2"]
            Sdots[...]
            Sn["Server M<br>10.10.0.(M+1)"]
        end

        %% Connections
        Clients -- "N Clients" --- F_Bus
        F_Bus --- LB

        LB --- B_Bus
        B_Bus -- "M Servers" --- Servers
    ```
    *(Diagram showing Clients connected to Frontend LAN Bus, Frontend LAN Bus connected to Load Balancer, Load Balancer connected to Backend LAN Bus, and Backend LAN Bus connected to Servers)*

* **Traffic:** Clients implement a request-response application over TCP. They send requests of a configurable size (`reqSize`) at a configurable interval (`reqInterval`) for a specific number of requests (`reqCount`) or continuously if `reqCount` is 0. Each request includes a custom header (`RequestResponseHeader`) containing:
    * Sequence Number: For tracking requests and responses.
    * Timestamp: Used by the client to calculate end-to-end latency upon receiving the response.
    * Payload Size: Indicates the size of the application data following the header (used for framing).
    * L7 Identifier: A unique 64-bit identifier per request (generated randomly by the client) used for consistent hashing algorithms (RingHash, Maglev).

* **Backend Servers:** Servers run a simple application that receives requests, potentially introduces a configurable processing delay (`serverDelays`), and echoes the request header back as the response.

* **Load Balancing Algorithms Implemented:** The load balancer application (`LoadBalancerApp`) is implemented as a Layer 7 TCP proxy. The following algorithms are available via the `lbAlgorithm` command-line argument:
    * `WRR`: Weighted Round Robin. Distributes requests sequentially based on assigned backend weights.
    * `LR`: Least Request. Uses Power-of-Two-Choices (P2C) to select the backend with fewer active requests (based on the base class's L7 request counter) when weights are equal. Uses a dynamic weighted algorithm (inspired by Envoy) when weights differ, factoring in active requests and weight.
    * `Random`: Selects a backend uniformly at random from the available pool.
    * `RingHash`: Ketama-style consistent hashing based on the request's L7 identifier. Backend placement on the ring is weighted.
    * `Maglev`: Google's Maglev consistent hashing algorithm based on the request's L7 identifier. Backend placement in the lookup table is weighted.
    * `PeakEWMA`: Uses P2C selection, choosing the backend with the lower Peak EWMA score. The score is based on an exponentially weighted moving average of backend request latency, penalized by the number of outstanding requests, and is particularly sensitive to latency peaks.

* **Metrics Collected:**
    * **End-to-End Latency:** Measured by each client from the time a request is sent until the corresponding response is fully received. Statistics (Min, Avg, Max, Percentiles, Std Dev) are calculated across all received responses from all clients.
    * **Server Request Distribution:** The total number of requests processed by each backend server is tracked and reported at the end of the simulation.

### Execution Model

This ns-3 simulation employs a discrete-event and single-threaded execution model.

- **Discrete-Event**: The simulation advances by processing a queue of scheduled events (like packet arrivals or timer expirations) at specific simulated times.
- **Single-Threaded**: All simulation logic, including network operations and application behaviors, is executed sequentially within a single thread. The `Simulator::Run()` function manages this event loop. The core simulation engine is deterministic given an identical sequence of events and inputs. 

## Development Environment with Docker

This project uses Docker to provide a consistent and reproducible development and build environment for the ns-3 simulation.

### Prerequisites

* Docker installed and running. (https://www.docker.com/get-started)
* `make` utility.

### Project Structure

* `load-balancer-simulation/`: Contains all custom C++ source (.cc), header (.h) files, and the `CMakeLists.txt` specific to this ns-3 module. The main simulation executable (`main.cc`) is also defined here.
* `Dockerfile.dev`: Defines the Docker image for interactive development. It includes ns-3 dependencies and sources but does not build ns-3 itself. Based on `ubuntu:22.04`.
* `Dockerfile.build`: Defines the Docker image for the final simulation binary. It uses the `Dockerfile.dev` image as a base, copies the `load-balancer-simulation/` module into the ns-3 source tree, and then builds ns-3 including the custom module.
* `Makefile`: Provides helper commands for building Docker images, managing the development environment, and running the simulation.
* `README.md`: This file.
* `.gitignore`: Specifies intentionally untracked files (e.g., `build_cache/`).
* `build_cache/`: (Git-ignored, created by `make`) This directory is mounted into the dev container to cache ns-3 build artifacts, speeding up incremental compilation.

### Development Workflow

1.  **Build the Development Image**:
    This image contains all necessary dependencies and the ns-3 source code.
    ```bash
    make build-dev-image
    ```

2.  **Start an Interactive Development Shell**:
    This command starts a Docker container based on the development image. Your local `load-balancer-simulation/` directory will be mounted into the container at `/usr/src/ns-allinone/ns-3.44/src/load-balancer-simulation`, and your local `build_cache/` directory will be mounted to `/usr/src/ns-allinone/ns-3.44/build`. Changes to your module code on your host are immediately reflected in the container, and ns-3 build artifacts are persisted in `build_cache/`.

    ```bash
    make shell-dev
    ```

3.  **Inside the Development Shell**:
    You'll be dropped into a bash shell inside the container, in the `${NS3_DIR}` directory (`/usr/src/ns-allinone/ns-3.44/`).

    * **Initial Configuration (if needed)**:
        The first time you start the shell, or if you've made changes to the `CMakeLists.txt` in your `load-balancer-simulation/` module, you need to configure ns-3.
        ```bash
        # (You should already be in /usr/src/ns-allinone/ns-3.44/)
        ns3-conf
        ```
        This is an alias for `./ns3 configure --build-profile=debug --disable-python --enable-examples --out=./build`. The `--out=./build` ensures output goes to the mounted `build_cache/` directory.

    * **Incremental Builds**:
        To compile your custom module and the rest of ns-3:
        ```bash
        ns3-bld
        ```
        This is an alias for `./ns3 build`. Since the `build` directory is mounted from your host's `build_cache/`, subsequent builds will be incremental.

    * **Running Your Executable (for quick tests inside dev)**:
        After a successful build (e.g., with `ns3-bld`), you can use the `ns3-run-sim` alias to run your simulation:
        ```bash
        ns3-run-sim
        ```
        This alias points to `./build/src/load-balancer-simulation/examples/ns3.44-main-debug` (assuming ns-3.44).

    * **Exiting the Shell**: Type `exit`.

4.  **Non-Interactive Configure/Build (Alternative to `shell-dev`)**
    You can run configure and build commands directly from your host, which uses a background dev container:
    ```bash
    make configure-ns3  # Runs './ns3 configure ...'
    make build-ns3      # Runs './ns3 build'
    ```
    Stop the background container with `make stop-dev` when done.

### Building the Final Simulation Image

To create a self-contained Docker image with your compiled simulation:
```bash
make build-sim
