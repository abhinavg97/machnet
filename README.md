# Machnet

Machnet is an ongoing project to provide an easy way for applications to access
low-latency userspace networking like DPDK. Machnet runs as a separate process
on all machines where the application is deployed and manages networking.
Applications interact with Machnet over shared memory with a familiar
socket-like API. Machnet processes in the cluster communicate with each other
using userspace networking.

Machnet provides the following benefits, in addition to the low latency:

- Designed specifically for cloud VM environments like Azure.
- Multiple applications can simultaneously use the same network interface.
- No need for DPDK expertise, or compiling the application with DPDK.

# Steps to use Machnet on Azure

## 1. Set up two VMs, each with two accelerated NICs

Machnet requires a dedicated NIC on each VM that it runs on. This NIC may be
used by multiple applications that use Machnet.

We recommend the following steps:

  1. Create two VMs on Azure, each with accelerated networking enabled. The VMs will start up with one NIC each, named `eth0`. This NIC is *never* used by Machnet.
  2. Shut-down the VMs.
  3. Create two new accelerated NICs from the portal, with no public IPs, and add one to each VM. Then restart the VMs. Each VM should now have another NIC named `eth1`, which will be used by Machnet.


## 2. Get the Machnet Docker image

The Machnet binary is provided in the form of a Docker image on Github container
registry. Pulling public images from Github container registry requires a few
mandatory steps.

 1. Generate a Github personal access token for yourself (https://github.com/settings/tokens) with the read:packages scope. and store it in the `GITHUB_PAT` environment variable.
 2. At `https://github.com/settings/tokens`, follow the steps to "Configure SSO" for this token.

```bash
# Install packages required to try out Machnet
sudo apt-get update
sudo apt-get install -y docker.io make cmake gcc pkg-config g++ uuid-dev libgflags-dev net-tools driverctl jq

# Reboot like below to allow non-root users to run Docker
sudo usermod -aG docker $USER && sudo reboot

# We assume that the Github token is stored as GITHUB_PAT
echo ${GITHUB_PAT} | docker login ghcr.io -u <github_username> --password-stdin
docker pull ghcr.io/microsoft/machnet/machnet:latest
```

## 3. Start the Machnet service on both VMs

Using DPDK on Azure requires unbinding the second NIC (`eth1` here ) from the
OS, which will cause this NIC to disappear from tools like `ifconfig`. **Before
this step, note down the IP and MAC address of the NIC, since we will need them
later.**

```bash
MACHNET_IP_ADDR=`ifconfig eth1 | grep -w inet | tr -s " " | cut -d' ' -f 3`
MACHNET_MAC_ADDR=`ifconfig eth1 | grep -w ether | tr -s " " | cut -d' ' -f 3`

# Unbind eth1 from the OS
sudo modprobe uio_hv_generic
DEV_UUID=$(basename $(readlink /sys/class/net/eth1/device))
sudo driverctl -b vmbus set-override $DEV_UUID uio_hv_generic

# Start Machnet
echo "Machnet IP address: $MACHNET_IP_ADDR, MAC address: $MACHNET_MAC_ADDR"
git clone --recursive https://github.com/microsoft/machnet.git
cd machnet
./machnet.sh --mac $MACHNET_MAC_ADDR --ip $MACHNET_IP_ADDR

# Note: If you lose the NIC info, the Azure metadata server has it:
curl -s -H Metadata:true --noproxy "*" "http://169.254.169.254/metadata/instance?api-version=2021-02-01" | jq '.network.interface[1]'
```

## 4. Run the hello world example

At this point, the Machnet container/process is running on both VMs. We can now
test things end-to-end with a client-server application.

```bash
# Build the Machnet helper library and hello_world example, on both VMs
./build_shim.sh
cd hello_world; make

# On VM #1, run the hello_world server
./hello_world --local <eth1 IP address of VM 1>

# On VM #2, run the hello_world client
./hello_world --local <eth1 IP address of VM 1> --remote <eth1 IP address of VM 2>
```

## 5. Run the end-to-end benchmark

```bash
# Build the `msg_gen` app for benchmarking. This partial build does not need DPDK or rdma_core.
cd machnet
rm -rf build; mkdir build; cd build; cmake -DCMAKE_BUILD_TYPE=Release ..; make -j

# See available benchmark options
./src/apps/msg_gen/msg_gen --help

# On VM #1, run the msg_gen server
./src/apps/msg_gen/msg_gen --local_ip <eth1 IP address of VM 1> --logtostderr

# On VM #2, run the msg_gen client
./src/apps/msg_gen/msg_gen --local_ip <eth1 IP address of VM 1> --remote_ip <eth1 IP address of VM 2> --active_generator --logtostderr
```

The client should print message rate and latency percentile statistics.
`msg_gen --help` lists all the options available (e.g., message size, number of outstanding messages, etc.).


## Application Programming Interface
Applications use the following steps to interact with the Machnet service:

- Initialize the Machnet library using `machnet_init()`.
- In every thread, create a new channel to Machnet using `machnet_attach()`.
- Listen on a port using `machnet_listen()`.
- Connect to remote processes using `machnet_connect()`.
- Send and receive messages using `machnet_send()` and `machnet_recv()`.

See [machnet.h](src/ext/machnet.h) for the full API documentation.

## Developing Machnet

See [CONTRIBUTING.md](CONTRIBUTING.md). for instructions on how to build and test Machnet.
