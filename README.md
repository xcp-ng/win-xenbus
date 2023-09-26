XenBus - The Xen Paravitual Bus Device Driver for Windows
=========================================================

The XenBus package consists of three device drivers:

*    xenbus.sys is a bus driver which attaches to a virtual device on the PCI
     bus and provides child devices for the other paravirtual device drivers
     to attach to.

*    xen.sys is a support library which provides interfaces for communicating
     with the Xen hypervisor

*    xenfilt.sys is a filter driver which is used to handle unplugging of
     emulated devices (such as disk and network devices) when paravirtual
     devices are available 

Quick Start Guide
=================

Building the driver
-------------------

See BUILD.md

Installing the driver
---------------------

See INSTALL.md

Driver Interfaces
=================

See INTERFACES.md

Miscellaneous
=============

For convenience the source repository includes some other scripts:

get_xen_headers.py
------------------

This will import any necessary headers from a given tag of that Xen
repository at git://xenbits.xen.org/xen.git.

System Start Options
====================

Behaviour in both the xen.sys and xenbus.sys can be configured by 'system
start options'. These options can be supplied on the Windows loader 'command
line', which can be set by hitting F10 during early boot or appended to by
setting the value of 'loadptions' using bcdedit.

XEN:DBG_PRINT=ON|OFF (default: ON)

This option determines with DbgPrint() output is intercepted and logged.
(To reduce noise, messages not prefixed with 'XEN' are ignored).

XEN:BOOT_EMULATED=TRUE|FALSE (default: FALSE)

This option avoids unplugging the first emulated IDE device, which is
useful when debugging the XENVBD driver (since the system disk remains
emulated).

XEN:BALLOON=ON|OFF (default: ON)

This option controls whether the XENBUS_BALLOON interface and thread is
enabled.

XEN:WATCHDOG=<TIME-OUT> (default: 0 minimum: 10)

This options determine whether the domain watchdog is enabled. If a non-zero
time-out (in seconds) is specified then the watchdog will be enabled. The
watchdog is patted by VIRQ_TIMER handlers and hence this is useful to
detect length stalls in event delivery or handling. The minimum time-out
value is 10s.
