#!/bin/bash

# Script to build and run FsEventBridge in Docker with necessary privileges

IMAGE_NAME="fseventbridge"
MONITOR_DIR="/tmp/docker_test"

# Create a test directory on the host
mkdir -p $MONITOR_DIR

echo "Building Docker image..."
docker build -t $IMAGE_NAME .

echo "Running FsEventBridge in privileged container..."
echo "Monitoring host directory: $MONITOR_DIR"
echo "Events will be sent to internal socket (use docker exec to see logs)"

# Run with --privileged to allow fanotify
docker run -d \
  --name feb_test \
  --privileged \
  -v $MONITOR_DIR:/monitor \
  $IMAGE_NAME

echo "Container feb_test is running."
echo "You can check logs with: docker logs -f feb_test"
echo "Try creating a file in $MONITOR_DIR to trigger events."
