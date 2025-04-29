# Makefile: ns-3 Docker Simulation Environment

# Configuration
NS3_VERSION := 3.44
DEV_IMAGE_NAME := ns3-load-balancer-dev
DEV_IMAGE_TAG := ${NS3_VERSION}
BUILD_IMAGE_NAME := ns3-load-balancer-sim
BUILD_IMAGE_TAG := ${NS3_VERSION}

# Host paths use CURDIR for portability
# Path to your custom ns-3 module on the host
NS3_MODULE_DIR_HOST := $(CURDIR)/load-balancer-simulation
# Path where the custom module will be mounted inside the container's ns-3 src directory
NS3_MODULE_DIR_CONTAINER := /usr/src/ns-allinone/ns-${NS3_VERSION}/src/load-balancer-simulation

# Path for the ns-3 build output cache on the host
NS3_BUILD_DIR_HOST := $(CURDIR)/build_cache
# Path for the ns-3 build output inside the container
NS3_BUILD_DIR_CONTAINER := /usr/src/ns-allinone/ns-${NS3_VERSION}/build

# Name for the running dev container
DEV_CONTAINER_NAME := ns3-dev-interactive-shell

# Default ns-3 source directory inside the container
NS3_SRC_DIR_CONTAINER := /usr/src/ns-allinone/ns-${NS3_VERSION}

# Arguments to pass to the simulation run
# Default is empty; set via 'make run-sim SIM_ARGS="..."'
# These are passed as separate arguments to the entrypoint.
SIM_ARGS ?=
# Example: SIM_ARGS = --param1=value1 --param2=value2

# Extra arguments for docker build (e.g., --build-arg CACHE_BUSTER=$(shell date +%s))
DOCKER_BUILD_EXTRA_ARGS ?=

.PHONY: all build-dev-image shell-dev start-dev-bg stop-dev configure-ns3 build-ns3 build-sim run-sim clean-build-cache clean-docker help

all: help

# Build the development Docker image
build-dev-image:
	@echo "Building ns-3 development image: ${DEV_IMAGE_NAME}:${DEV_IMAGE_TAG}..."
	docker build \
		--build-arg NS3_VERSION=${NS3_VERSION} \
		${DOCKER_BUILD_EXTRA_ARGS} \
		-t ${DEV_IMAGE_NAME}:${DEV_IMAGE_TAG} \
		-f Dockerfile.dev .

# Start an interactive shell in the dev container
# Mounts custom module and build cache.
shell-dev: build-dev-image stop-dev ## Start/enter interactive dev shell
	@mkdir -p ${NS3_BUILD_DIR_HOST}
	@echo "Starting interactive dev container: ${DEV_CONTAINER_NAME}..."
	@echo "Mounting custom module: ${NS3_MODULE_DIR_HOST} -> ${NS3_MODULE_DIR_CONTAINER}"
	@echo "Mounting build cache:   ${NS3_BUILD_DIR_HOST} -> ${NS3_BUILD_DIR_CONTAINER}"
	@echo "Once inside, use 'ns3-conf' to configure and 'ns3-bld' to build."
	# Use $(CURDIR) directly in the volume mount for robustness
	docker run -it --rm \
		--name ${DEV_CONTAINER_NAME} \
		-v "$(CURDIR)/load-balancer-simulation:${NS3_MODULE_DIR_CONTAINER}" \
		-v "$(CURDIR)/build_cache:${NS3_BUILD_DIR_CONTAINER}" \
		${DEV_IMAGE_NAME}:${DEV_IMAGE_TAG} bash

# Start dev container in background (for exec commands)
start-dev-bg: build-dev-image ## Start dev container in background
	@if ! docker ps -q -f name=$(DEV_CONTAINER_NAME) > /dev/null; then \
		mkdir -p ${NS3_BUILD_DIR_HOST}; \
		echo "Starting dev container ${DEV_CONTAINER_NAME} in background..."; \
		docker run -d --rm \
			--name ${DEV_CONTAINER_NAME} \
			-v "$(CURDIR)/load-balancer-simulation:${NS3_MODULE_DIR_CONTAINER}" \
			-v "$(CURDIR)/build_cache:${NS3_BUILD_DIR_CONTAINER}" \
			${DEV_IMAGE_NAME}:${DEV_IMAGE_TAG} tail -f /dev/null; \
	else \
		echo "Dev container ${DEV_CONTAINER_NAME} is already running."; \
	fi

# Stop the detached dev container
stop-dev: ## Stop the background dev container
	@echo "Attempting to stop dev container ${DEV_CONTAINER_NAME} (if running)..."
	-docker stop ${DEV_CONTAINER_NAME} > /dev/null 2>&1 || true

# Configure ns-3 inside the running dev container
configure-ns3: start-dev-bg ## Configure ns-3 in dev container
	@echo "Configuring ns-3 in ${DEV_CONTAINER_NAME} (profile: debug, no python, examples enabled)..."
	# Using --out=build to match the volume mount and dev alias behavior
	docker exec -w ${NS3_SRC_DIR_CONTAINER} ${DEV_CONTAINER_NAME} ./ns3 configure --build-profile=debug --disable-python --enable-examples --out=build

# Build ns-3 inside the running dev container
build-ns3: start-dev-bg ## Build ns-3 in dev container
	@echo "Building ns-3 in ${DEV_CONTAINER_NAME}..."
	docker exec -w ${NS3_SRC_DIR_CONTAINER} ${DEV_CONTAINER_NAME} ./ns3 build

# Build the final simulation image
build-sim: build-dev-image ## Build the final simulation image
	@echo "Building final simulation image: ${BUILD_IMAGE_NAME}:${BUILD_IMAGE_TAG}..."
	docker build \
		--build-arg NS3_VERSION=${NS3_VERSION} \
		--build-arg DEV_IMAGE_NAME=${DEV_IMAGE_NAME} \
		--build-arg DEV_IMAGE_TAG=${DEV_IMAGE_TAG} \
		-t ${BUILD_IMAGE_NAME}:${BUILD_IMAGE_TAG} \
		-f Dockerfile.build .

# Run the simulation using the final build image
run-sim: build-sim ## Run the ns-3 simulation with specified ARGS
	@echo "Running simulation with image ${BUILD_IMAGE_NAME}:${BUILD_IMAGE_TAG} and args: ${SIM_ARGS}"
	docker run --rm ${BUILD_IMAGE_NAME}:${BUILD_IMAGE_TAG} ${SIM_ARGS}

# Clean the local build cache
clean-build-cache: ## Remove local ns-3 build cache
	@echo "Cleaning local build cache directory: ${NS3_BUILD_DIR_HOST}..."
	rm -rf ${NS3_BUILD_DIR_HOST}

# Clean Docker images
clean-docker: stop-dev ## Remove built Docker images
	@echo "Removing Docker images..."
	-docker rmi ${BUILD_IMAGE_NAME}:${BUILD_IMAGE_TAG} > /dev/null 2>&1 || true
	-docker rmi ${DEV_IMAGE_NAME}:${DEV_IMAGE_TAG} > /dev/null 2>&1 || true

# Help target to display available commands
help:
	@echo "Available commands:"
	@echo "  make build-dev-image         Build the ns-3 development Docker image."
	@echo "                               Use 'make build-dev-image DOCKER_BUILD_EXTRA_ARGS=\"--build-arg CACHE_BUSTER=\`date +%s\`\"' to force apt refresh."
	@echo "  make shell-dev               Start an interactive shell in the dev container (mounts sources and build cache)."
	@echo "  make start-dev-bg            Start the dev container in the background."
	@echo "  make stop-dev                Stop the background dev container."
	@echo "  make configure-ns3           Run './ns3 configure' inside the (background) dev container."
	@echo "  make build-ns3               Run './ns3 build' inside the (background) dev container."
	@echo "  make build-sim               Build the final simulation Docker image."
	@echo "  make run-sim                 Run the simulation (use 'make run-sim SIM_ARGS=\"--your --args\"')."
	@echo "  make clean-build-cache       Remove the local build_cache/ directory."
	@echo "  make clean-docker            Remove the built Docker images."
	@echo "  make help                    Show this help message."
	@echo ""
	@echo "Development Workflow:"
	@echo "  1. Build dev image:          'make build-dev-image'"
	@echo "  2. Start interactive shell:  'make shell-dev'"
	@echo "     Inside the shell:"
	@echo "       \$ cd /usr/src/ns-allinone/ns-${NS3_VERSION}/"
	@echo "       \$ ns3-conf  # (first time or after major changes to custom module CMakeLists.txt)"
	@echo "       \$ ns3-bld   # (for incremental builds)"
	@echo "       \$ exit"
	@echo "  Alternatively, for non-interactive configure/build:"
	@echo "  'make configure-ns3' then 'make build-ns3' (uses a background container)"
	@echo "  3. Build final image:        'make build-sim'"
	@echo "  4. Run simulation:           'make run-sim SIM_ARGS=\"--numClients=X ...\"'"