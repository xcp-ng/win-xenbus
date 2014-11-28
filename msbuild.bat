call "%VS%\VC\vcvarsall.bat" x86
@echo on

cov-build --dir cov_dir msbuild.exe /m:1 /p:Configuration="%CONFIGURATION%" /p:Platform="%PLATFORM%" /t:"%TARGET%" %EXTRA% %FILE%

if errorlevel 1 goto error

cov-analyze --dir cov_dir -j auto --aggressiveness-level high --security --concurrency --preview --all --rule --enable-fnptr --strip-path c:\git\xenbus

cov-commit-defects --dir cov_dir --host dagu-4.uk.xensource.com --port 8080 --user windows --password coverity --stream %PLATFORM%


exit 0

:error
exit 1
