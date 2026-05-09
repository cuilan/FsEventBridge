#!/bin/bash

# Script Name: setup_env.sh
# Description: Initialize development environment for FsEventBridge (fseventbridge binary).
#              Supports Debian/Ubuntu, RHEL/Fedora/CentOS, Arch Linux, and openSUSE.

# Color definitions for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

set -e

echo -e "${GREEN}=== FsEventBridge dev environment setup (fseventbridge) ===${NC}"

# 1. Check for root privileges
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Please run as root (e.g., using sudo)${NC}"
  exit 1
fi

# 2. Detect Operating System
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID=$ID
    OS_LIKE=$ID_LIKE
else
    echo -e "${RED}Cannot detect operating system. /etc/os-release not found.${NC}"
    exit 1
fi

echo -e "${GREEN}[1/5] Detected System: $PRETTY_NAME${NC}"

# 3. Install dependencies based on distribution
echo -e "${GREEN}[2/5] Installing build tools and dependencies...${NC}"

case "$OS_ID" in
    ubuntu|debian|raspbian|kali)
        apt-get update
        apt-get install -y build-essential cmake pkg-config liburing-dev libsystemd-dev
        ;;
    fedora|rhel|centos|rocky|almalinux)
        # For RHEL/CentOS/Rocky, EPEL might be needed for liburing
        if [[ "$OS_ID" != "fedora" ]]; then
            echo -e "${YELLOW}Attempting to enable EPEL repository...${NC}"
            dnf install -y epel-release || true
        fi
        dnf install -y gcc gcc-c++ cmake make pkgconfig liburing-devel systemd-devel
        ;;
    arch|manjaro)
        pacman -Sy --noconfirm --needed base-devel cmake liburing systemd
        ;;
    opensuse*|suse)
        zypper refresh
        zypper install -y gcc gcc-c++ cmake make pkg-config liburing-devel systemd-devel
        ;;
    *)
        # Check ID_LIKE for derivatives
        if [[ "$OS_LIKE" == *"debian"* ]]; then
            apt-get update
            apt-get install -y build-essential cmake pkg-config liburing-dev libsystemd-dev
        elif [[ "$OS_LIKE" == *"rhel"* || "$OS_LIKE" == *"fedora"* ]]; then
            dnf install -y gcc gcc-c++ cmake make pkgconfig liburing-devel systemd-devel
        elif [[ "$OS_LIKE" == *"arch"* ]]; then
            pacman -Sy --noconfirm --needed base-devel cmake liburing systemd
        else
            echo -e "${RED}Unsupported distribution: $OS_ID${NC}"
            echo -e "Please manually install: gcc, cmake, liburing, libsystemd-dev${NC}"
            exit 1
        fi
        ;;
esac

# 4. Verify kernel version (fanotify requires >= 5.1)
echo -e "${GREEN}[3/5] Checking kernel version...${NC}"
KERNEL_VERSION=$(uname -r | cut -d. -f1,2)
REQUIRED_VERSION="5.1"

if [ "$(printf '%s\n' "$REQUIRED_VERSION" "$KERNEL_VERSION" | sort -V | head -n1)" = "$REQUIRED_VERSION" ]; then
    echo -e "${GREEN}Kernel version $KERNEL_VERSION meets requirements.${NC}"
else
    echo -e "${YELLOW}Warning: Kernel version $KERNEL_VERSION is lower than recommended $REQUIRED_VERSION.${NC}"
    echo -e "${YELLOW}Some fanotify features might not work as expected.${NC}"
fi

# 5. Verify development environment
echo -e "${GREEN}[4/5] Verifying installation...${NC}"

# Check GCC
if command -v gcc >/dev/null 2>&1; then
    echo -e "${GREEN}✓ GCC is ready: $(gcc --version | head -n1)${NC}"
else
    echo -e "${RED}✗ GCC installation failed${NC}"
fi

# Check libraries
MISSING_LIB=0
# Use pkg-config to check for libraries as it's more reliable across distros
if pkg-config --exists liburing; then
    echo -e "${GREEN}✓ liburing is ready${NC}"
else
    echo -e "${RED}✗ liburing not found (via pkg-config)${NC}"
    MISSING_LIB=1
fi

if pkg-config --exists libsystemd; then
    echo -e "${GREEN}✓ libsystemd is ready${NC}"
else
    echo -e "${RED}✗ libsystemd not found (via pkg-config)${NC}"
    MISSING_LIB=1
fi

# 6. Final Report
echo -e "${GREEN}==================================================${NC}"
if [ $MISSING_LIB -eq 0 ]; then
    echo -e "${GREEN}Environment setup complete!${NC}"
else
    echo -e "${YELLOW}Setup finished with some warnings. Please check the logs above.${NC}"
fi
echo -e "${YELLOW}Note: If running in containers (LXC, Docker, etc.):${NC}"
echo -e "${YELLOW}      Ensure the container is privileged or has CAP_SYS_ADMIN capabilities.${NC}"
echo -e "${GREEN}==================================================${NC}"
