call d:\BuildEnv\SetupBuildEnv.cmd
set
set SDVBINPATH=%ToolsPath%\sdv\bin
set PATH=%PATH%;%SDVBINPATH%
powershell -Command .\build.ps1 free -sdv
7z a -ttar xenbus.tar xenbus
