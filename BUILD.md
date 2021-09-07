Building the XenBus Package
===========================

First you'll need a device driver build environment for Windows 10. Happily
Microsoft has made this easy with the introduction of the 'EWDK'. This is an
ISO containing all the build environment you need.

The package should support building with the following EWDKs:

- EWDK for Server 2022, version 22000 with Visual Studio Build Tools 16.9
- EWDK for Windows 10, version 1903 with Visual Studio Build Tools 16.0
- EWDK for Windows 10, version 1809 with Visual Studio Build Tools 15.8.9

Once you have downloaded the ISO, open it and you should see a file called:

LaunchBuildEnv.cmd

Run this and it should give you a build environment command prompt. From
within this shell navigate to the root of your checked out repository
and run:

powershell ./build.ps1

This will then prompt you for whether you want a 'free' (non-debug) or a
'checked' (debug) build and then proceed to build all x86 and x64 drivers.

NOTE: Because the EWDKs do not contain the 'dpinst' re-distributable driver
installer utility, this will not be included in the built driver package
by default. However, if you set the environment variable DPINST_REDIST to
point to a directory with x86 and x64 sub-directories containing 32- and
64-bit dpinst.exe binaries (respectively) then these will be copied into
the built packages, making installation more convenient.

NOTE: In order to use the '-CodeQL' parameter to generate *.sarif log files,
an additional tool and set of rules will need installing. The CodeQL engine
can be downloaded from https://github.com/github/codeql-cli-binaries/releases
and the driver specific rules can be cloned from
https://github.com/microsoft/Windows-Driver-Developer-Supplemental-Tools.
Once acquired, the rules need to be in a sibling folder of the engine (e.g.
C:\Tools\CodeQL and C:\Tools\Windows-Driver-Developer-Supplemental-Tools) and
the CodeQL engine (e.g. C:\Tools\CodeQL) must be added to the PATH environment
variable. Further information available at
https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/static-tools-and-codeql
