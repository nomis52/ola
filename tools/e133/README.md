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

## basic_device

This uses DNS-SD to locate the '_rdmnet-ctrl._tcp' services and looks for the
key 'priority' in the TXT record. The priority should be between 0 and 100. It
then attempts to open a health checked TCP connection to a controller, starting
with the controller with the highest priority.

The --controller-address argument can be used to override DNS-SD and connect to
a single controller.

## basic_controller

This listens for TCP connections from devices. It does not yet register with
DNS-SD.

To perform scale testing you may need to increase the number of FDs per process.

### Mac:

> sysctl -w kern.maxfiles=20480
> sysctl -w kern.maxfilesperproc=18000
> ulimit -n 12000
> ulimit -a  # confirm
