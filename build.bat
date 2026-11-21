@echo off
echo ============================
echo   Building ClockOS...
echo ============================

if not exist build mkdir build

echo.
echo [1/7] Assembling Boot (boot.asm)...
nasm -f bin boot.asm -o build\boot.bin
if errorlevel 1 goto :error

echo [2/7] Assembling Loader (loader.asm)...
nasm -f bin loader.asm -o build\loader.bin
if errorlevel 1 goto :error

echo [3/7] Assembling Kernel (kernel.asm)...
nasm -f elf32 kernel.asm -o build\kernel_start.o
if errorlevel 1 goto :error

echo [4/7] Assembling Kernel C (kernel.c)...
x86_64-elf-gcc -m32 -mno-80387 -mno-sse -mno-mmx -ffreestanding -nostdlib -c kernel.c -o build\kernel.o
if errorlevel 1 goto :error

echo [5/7] Linking kernel...
x86_64-elf-ld -m elf_i386 -T linker.ld  -o build\kernel.elf build\kernel_start.o build\kernel.o 
x86_64-elf-objcopy -O binary build\kernel.elf build\kernel.bin
if errorlevel 1 goto :error

echo [6/7] Creating empty 1.44MB floppy image and writing stages...
fsutil file createnew build\ClockOS.img 1474560 >nul
if errorlevel 1 goto :error

echo [7/7] Writing Boot + Loader + Kernel to disk image...
:: 写入 Boot 到第0扇区 (MBR)
dd if=build\boot.bin of=build\ClockOS.img bs=512 count=1 conv=notrunc 2>nul
if errorlevel 1 (
    echo ERROR: dd command failed! Make sure 'dd' is installed and in PATH.
    goto :error
)

:: 写入 Loader 从第1扇区
dd if=build\loader.bin of=build\ClockOS.img bs=512 seek=1 conv=notrunc 2>nul
if errorlevel 1 (
    echo ERROR: dd command failed!
    goto :error
)

dd if=build\kernel.bin of=build\ClockOS.img bs=512 seek=17 conv=notrunc 2>nul
if errorlevel 1 (
    echo ERROR: dd command failed!
    goto :error
)

:: 重要提示：请检查 Kernel 的大小！
for %%F in (build\kernel.bin) do set kernelSize=%%~zF
echo.
echo kernel size: %kernelSize% bytes
echo NOTE: The Loader uses LBA to read Kernel.
echo If kernel grows larger, adjust the number in 'dap_packet' inside loader.asm

echo.
echo ============================
echo   Build successful!
echo   Image: build\ClockOS.img
echo ============================
goto :end

:error
echo.
echo BUILD FAILED!
pause
exit /b 1

:end
pause