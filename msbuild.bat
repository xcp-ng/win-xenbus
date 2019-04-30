set MSBUILD_ROOT=%cd%
call "%MSBUILD_VCVARSALL%" x86_amd64
@echo on
cd "%MSBUILD_ROOT%"
msbuild.exe /m:1 /p:Configuration="%MSBUILD_CONFIGURATION%" /p:Platform="%MSBUILD_PLATFORM%" /t:"%MSBUILD_TARGET%" %MSBUILD_EXTRA% %MSBUILD_FILE%
if errorlevel 1 goto error
exit 0

:error
exit 1
