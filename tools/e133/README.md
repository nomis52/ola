# E1.33 Sandbox

This directory contains programs for prototyping early (pre-standardization)
ideas for E1.33 (RDMNet).

# Gen I (SLP based)

The original version of E1.33 used SLP for discovery. Since OpenSLP was
problematic, Simon N wrote an OLA version of SLP, including a simple Directory
Agent (DA). This is known as OLA SLP and should be compatible with other SLP
implementations, with the exception of attributes.

To use these you need to be running the OLA slp_server: ./slp/slp_server.

After a controller discovers an E1.33 device, it attempts to become the
designated controller by opening a TCP connection to it. If successful, the
device then sends status messages and ack-timer responses to the controllers
via this TCP connection.

In this version of E1.33, controller syncronization is obtained with TCP
connections between controllers, otherwise known as the TCP mesh. The Gen I
programs do not implement the controller mesh.

## e133_controller

A very basic RDMNet controller. This services for the specified E1.33 device
using SLP, sends the specified command and waits for the response.

## e133_monitor

Discover E1.33 devices via SLP and attempt to open a TCP connection to each.
Then print out any RDM commands received on the TCP connections.

## e133_receiver

A simple Gen I E1.33 device. It announces its presence via SLP and waits for a
controller to send it UDP commands, or open a TCP connection.

## slp_locate

Attempt to discover E1.33 services "service:rdmnet-device". Uses the OLA slp
daemon.

## slp_register

Register a E1.33 device in SLP.

## slp_sa_test

Run various tests against and E1.33 SA (RDMNet device). See SLPSATestRunner.cpp
for the full set of tests.

# Gen II (DNS-SD based)

The fact that SLP uses a reserved port (427) means it can't work with mobile
OSs like iOS and Android. In Nov 2013 the task group started considering using
DNS-SD instead. We also reversed the discovery model, rather than controllers
discovering devices, the devices would discover the controllers and open a TCP
connection to one of them.

## gen2_device

This uses DNS-SD to locate the '_rdmnet-ctrl._tcp' services and looks for the
key 'priority' in the TXT record. The priority should be between 0 and 100. It
then attempts to open a health checked TCP connection to a controller, starting
with the controller with the highest priority. Once a TCP connection is setup,
it informs the controller of the UDP port to send RDMNet get/set to.

It also responds to RDMNet commands send via UDP.

The --controller-address argument can be used to override DNS-SD and connect to
a single controller.

## gen2_controller

Registers the E1.33 service with DNS-SD and then listens for TCP connections
from devices.

It also forms the TCP mesh with any other controllers it discovers.

To perform scale testing you may need to increase the number of FDs per process
& the number of processes.

### Mac:

> sysctl -w kern.maxfiles=30000
> sysctl -w kern.maxprocperuid=12000
> sysctl -w kern.maxfilesperproc=18000
> ulimit -n 12000
> ulimit -v 12000
> ulimit -a  # confirm

## Gen 2 TODO list

* Implement a controller which finds all devices, then sends a TCP_CONNS_STATS2
  message to each, and check the response matches what we know from the
  controller mesh.

# Troubleshooting

## Bonjour

To browse for controllers run:

dns-sd  -B _rdmnet-ctrl._tcp

If everything works fine from the local machine, but other machines can't
discover the service, check for -NoMulticastAdvertisements in
/System/Library/LaunchDaemons/com.apple.mDNSResponder.plist. See
http://support.apple.com/kb/HT3789

## Avahi

To browse for controllers run:

avahi-browse _rdmnet-ctrl._tcp