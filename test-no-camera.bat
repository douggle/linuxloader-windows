@echo off
:: Same as build-and-test-rawthrills.bat, but with the NameCam camera off
:: and graphics driver errors written to dist-win32\test-run.log.
set "LL_NO_CAMERA=1"
set "LL_GL_DEBUG=1"
call "%~dp0build-and-test-rawthrills.bat" %*
