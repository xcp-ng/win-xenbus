call %EWDK%\BuildEnv\SetupBuildEnv.cmd
powershell -Command .\build.ps1 free %SDV%
7z a -ttar xenbus.tar xenbus
