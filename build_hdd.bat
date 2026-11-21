@echo off
echo ============================
echo   Building ClockOS (HDD/U version)...
echo ============================

if not exist build mkdir build

echo.
echo [1/8] Assembling Boot (boot_hdd.asm)...
nasm -f bin boot_hdd.asm -o build\boot_hdd.bin
if errorlevel 1 goto :error

echo [2/8] Assembling Loader (loader.asm)...
nasm -f bin loader.asm -o build\loader.bin
if errorlevel 1 goto :error

echo [3/8] Assembling Kernel (kernel.asm)...
nasm -f elf32 kernel.asm -o build\kernel_start.o
if errorlevel 1 goto :error

echo [4/8] Assembling Kernel C (kernel.c)...
x86_64-elf-gcc -m32 -mno-80387 -mno-sse -mno-mmx -ffreestanding -nostdlib -c kernel.c -o build\kernel.o
if errorlevel 1 goto :error

echo [5/8] Assembling ATA Driver (ata.c)...
x86_64-elf-gcc -m32 -mno-80387 -mno-sse -mno-mmx -ffreestanding -nostdlib -c ata.c -o build\ata.o
if errorlevel 1 goto :error

echo [6/8] Linking kernel...
x86_64-elf-ld -m elf_i386 -T linker.ld -o build\kernel.elf build\kernel_start.o build\kernel.o build\ata.o
x86_64-elf-objcopy -O binary build\kernel.elf build\kernel.bin
if errorlevel 1 goto :error

echo [7/8] Creating 1.44MB floppy image (as disk image)...
if not exist build\ClockOS_hdd.img (
    fsutil file createnew build\ClockOS_hdd.img 1474560 >nul
)
if errorlevel 1 goto :error

echo [8/8] Writing Boot + Loader + Kernel...
dd if=build\boot_hdd.bin of=build\ClockOS_hdd.img bs=512 count=1 conv=notrunc 2>nul
dd if=build\loader.bin of=build\ClockOS_hdd.img bs=512 seek=1 conv=notrunc 2>nul

:: 把初始内核写到主区 LBA 18
dd if=build\kernel.bin of=build\ClockOS_hdd.img bs=512 seek=18 conv=notrunc 2>nul

for %%F in (build\kernel.bin) do set kernelSize=%%~zF
echo.
echo kernel size: %kernelSize% bytes
echo Output image: build\ClockOS_hdd.img
echo (Boot from HDD/U-disk with LBA mode)
echo.

echo ============================
echo   Build successful!
echo ============================
goto :end

:error
echo.
echo BUILD FAILED!
pause
exit /b 1

:end
pause