Building the XenBus Package
===========================

First you'll need a device driver build environment for Windows 10.
This means:

*   Visual Studio 2015 (Any SKU, including Express or Community)
*   Windows Driver Kit 10

Install Visual Studio first (you only need install MFC for C++) and then
the WDK. Set an environment variable called VS to the base of the Visual
Studio Installation (e.g. C:\Program Files\Microsoft Visual Studio 14.0) and
a variable called KIT to the base of the WDK
(e.g. C:\Program Files\Windows Kits\10). Also set an environment variable
called SYMBOL\_SERVER to point at a location where driver symbols can be
stored. This can be local directory e.g. C:\Symbols.

You will also need to acquire the DIFx re-distributable package from one
of the older WDKs (as it appears not to be present in WDK10), so that the
driver build can copy dpinst.exe into the output.
Set the environment variable DPINST_REDIST to the base dpinst directory
- the directory under which the x86 and x64 sub-directories containing
dpinst.exe can be found
(e.g. C:\Program Files (x86)\Windows Kits\8.1\Redist\DIFx\dpinst\EngMui)

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
