Building the XenBus Package
===========================

First you'll need a device driver build environment for Windows 8 or
Windows 8.1.
For Windows 8 this means:

*   Visual Studio 2012 (Professional or Ultimate)
*   Windows Driver Kit 8

For Windows 8.1 this means:

*   Visual Studio 2013 (Any SKU, including Express)
*   Windows Driver Kit 8.1

(See http://msdn.microsoft.com/en-us/windows/hardware/hh852365.aspx). You
may find it useful to install VirtualCloneDrive from http://www.slysoft.com
as Visual Studio is generally supplied in ISO form.

Install Visual Studio first (you only need install MFC for C++) and then
the WDK. Set an environment variable called VS to the base of the Visual
Studio Installation (e.g. C:\Program Files\Microsoft Visual Studio 12.0) and
a variable called KIT to the base of the WDK
(e.g. C:\Program Files\Windows Kits\8.1). Also set an environment variable
called SYMBOL\_SERVER to point at a location where driver symbols can be
stored. This can be local directory e.g. C:\Symbols.

Next you'll need a 3.x version of python (which you can get from
http://www.python.org). Make sure python.exe is somewhere on your default
path.

Now fire up a Command Prompt and navigate to the base of your git repository.
At the prompt type:

    build.py checked

This will create a debug build of the driver. To create a non-debug build
type:

    build.py free

Note that Static Driver Verifier is run by default as part of the build
process. This can be very time consuming. If you don't want to run the
verifier then you can add the 'nosdv' keyword to the end of your command
e.g.:

    build.py free nosdv
