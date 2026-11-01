@echo off
:: Same as build-and-test-rawthrills.bat (camera on), with graphics driver
:: errors written to dist-win32\test-run.log.
set "LL_GL_DEBUG=1"
call "%~dp0build-and-test-rawthrills.bat" %*
