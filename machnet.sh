#!/bin/bash
# Start the Machnet service on this machineA
# Usage: machnet.sh --mac <local MAC> --ip <local IP>
#  - mac: MAC address of the local DPDK interface
#  - ip: IP address of the local DPDK interface
#  - bare_metal: if set, will use local binary instead of Docker image
#  - debug: if set, will spawn a DEBUG stack instance instead of a prod one

LOCAL_MAC=""
LOCAL_IP=""
BARE_METAL=0
DEBUG=0
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -m|--mac)
            LOCAL_MAC="$2"
            shift
            shift
            ;;
        -i|--ip)
            LOCAL_IP="$2"
            shift
            shift
            ;;
        -b|--bare_metal)
            BARE_METAL=1
            shift
            ;;
        -d|--debug)
            DEBUG=1
            shift
            ;;
        *)
            echo "Unknown option $key"
            exit 1
            ;;
    esac
done

# Pre-flight checks
if [ -z "$LOCAL_MAC" ] || [ -z "$LOCAL_IP" ]; then
    echo "Please provide both local MAC and IP address"
    exit 1
fi

#
# Hugepage allocation
#

#Allocate memory for the first NUMA node
if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
then
    echo "Insufficient or no hugepages available"
    read -p "Do you want to allocate 1024 2MB hugepages? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]
    then
        echo "OK, continuing without allocating hugepages"
    else
        echo "Allocating 1024 hugepages"
        sudo bash -c "echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
        if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
        then
            echo "Failed to allocate hugepages"
            exit 1
        else
            echo "Successfully allocated 1024 hugepages on NUMA node0"
        fi
    fi
fi

# Allocate memory for the rest of the NUMA nodes, if any
for n in /sys/devices/system/node/node[1-9]; do
    if [ -d "$n" ]; then
        sudo bash -c "echo 1024 > $n/hugepages/hugepages-2048kB/nr_hugepages"
        if ! cat $n/meminfo | grep HugePages_Total | grep -q 1024
        then
            echo "Failed to allocate hugepages on NUMA `echo $n | cut -d / -f 6`"
            exit 1
        else
            echo "Successfully allocated 1024 hugepages on NUMA `echo $n | cut -d / -f 6`"
        fi
    fi
done


echo "Starting Machnet with local MAC $LOCAL_MAC and IP $LOCAL_IP"

if [ ! -d "/var/run/machnet" ]; then
    echo "Creating /var/run/machnet"
    sudo mkdir -p /var/run/machnet
fi

sudo bash -c "echo '{\"machnet_config\": {\"$LOCAL_MAC\": {\"ip\": \"$LOCAL_IP\"}}}' > /var/run/machnet/local_config.json"
echo "Created config for local Machnet, in /var/run/machnet/local_config.json. Contents:"
sudo cat /var/run/machnet/local_config.json

if [ $BARE_METAL -eq 1 ]; then
    echo "Starting Machnet in bare-metal mode"
    THIS_SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
    BUILD_DIR=="$THIS_SCRIPT_DIR/"
    if [ $DEBUG -eq 1 ]; then
        BUILD_DIR="$THIS_SCRIPT_DIR/build/debug_build"
    else
        BUILD_DIR="$THIS_SCRIPT_DIR/build/release_build"
    fi
    machnet_bin="${BUILD_DIR}/src/apps/machnet/machnet"

    if [ ! -f ${machnet_bin} ]; then
        echo "${machnet_bin} not found, please build Machnet first"
        exit 1
    fi

    sudo ${machnet_bin} --config_json /var/run/machnet/local_config.json --logtostderr=1
else
    if ! command -v docker &> /dev/null
    then
        echo "Please install docker"
        exit
    fi

    if ! groups | grep -q docker; then
        echo "Please add the current user to the docker group"
        exit
    fi

    echo "Checking if the Machnet Docker image is available"
    if ! docker pull ghcr.io/microsoft/machnet/machnet:latest
    then
        echo "Please make sure you have access to the Machnet Docker image at ghcr.io/microsoft/machnet/"
        echo "See Machnet README for instructions on how to get access"
    fi

    if [ $DEBUG -eq 1 ]; then
        echo "Starting Machnet in DEBUG mode"
        sudo docker run --privileged --net=host \
            -v /dev/hugepages:/dev/hugepages \
            -v /var/run/machnet:/var/run/machnet \
            ghcr.io/microsoft/machnet/machnet:latest \
            /root/machnet/debug_build/src/apps/machnet/machnet \
            --config_json /var/run/machnet/local_config.json \
            --logtostderr=1
    else
        echo "Starting Machnet in RELEASE mode"
        sudo docker run --privileged --net=host \
            -v /dev/hugepages:/dev/hugepages \
            -v /var/run/machnet:/var/run/machnet \
            ghcr.io/microsoft/machnet/machnet:latest \
            /root/machnet/release_build/src/apps/machnet/machnet \
            --config_json /var/run/machnet/local_config.json \
            --logtostderr=1
    fi
fi
