@echo off
echo ============================
echo   Building Update Disk (update.img)...
echo ============================

if exist build\update.img del build\update.img

fsutil file createnew build\update.img 1474560 >nul

dd if=build\kernel.bin of=build\update.img bs=512 seek=0 conv=notrunc 2>nul

echo.
echo Update disk generated: build\update.img
echo You can now mount this file as a second drive in VMware.
echo.

echo ============================
echo   Build successful!
echo ============================
pause