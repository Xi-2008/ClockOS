[BITS 16]
[ORG 0x7E00]

KERNEL_SEG      equ 0x0900
KERNEL_OFF      equ 0x0000
KERNEL_LBA      equ 17
KERNEL_SECS     equ 128

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [drive_number], dl

    mov si, msg_loading
    call print_string_16

    ; Reset disk
    xor ah, ah
    mov dl, [drive_number]
    int 0x13
    jc disk_error

    ; Probe extended read
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive_number]
    int 0x13
    jc  .chs_path
    cmp bx, 0xAA55
    jne .chs_path

    ; LBA path
    mov si, dap_packet
    mov di, 3
.lba_retry:
    mov ah, 0x42
    mov dl, [drive_number]
    int 0x13
    jnc read_success
    xor ah, ah
    mov dl, [drive_number]
    int 0x13
    dec di
    jnz .lba_retry

.chs_path:
    ; Get geometry
    mov ah, 0x08
    mov dl, [drive_number]
    int 0x13
    jc disk_error
    and cl, 0x3F
    mov [sectors_per_track], cl
    mov [num_heads], dh
    inc byte [num_heads]

    ; Setup CHS read
    mov word [cur_lba], KERNEL_LBA
    mov word [sectors_left], KERNEL_SECS
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx

.chs_read_loop:
    ; sectors_per_cyl = heads * spt
    movzx ax, byte [num_heads]
    movzx cx, byte [sectors_per_track]
    mul cx
    mov cx, ax

    ; cylinder = LBA / sectors_per_cyl
    mov ax, [cur_lba]
    xor dx, dx
    div cx
    mov [cur_cyl], al
    mov ax, dx

    ; head = rem / spt
    movzx cx, byte [sectors_per_track]
    xor dx, dx
    div cx
    mov [cur_head], al
    mov [cur_sector], dl
    inc byte [cur_sector]

    mov di, 3
.chs_retry:
    mov ah, 0x02
    mov al, 1
    mov ch, [cur_cyl]
    mov cl, [cur_sector]
    mov dh, [cur_head]
    mov dl, [drive_number]
    int 0x13
    jnc .chs_ok
    xor ah, ah
    mov dl, [drive_number]
    int 0x13
    dec di
    jnz .chs_retry
    jmp disk_error

.chs_ok:
    mov ax, es
    add ax, 0x20
    mov es, ax
    inc word [cur_lba]
    dec word [sectors_left]
    jnz .chs_read_loop

read_success:
    mov si, msg_ok
    call print_string_16

    ; 1. 读 BCB 到 0x4000
    mov word [dap_packet + 2], 1
    mov word [dap_packet + 4], 0x4000
    mov word [dap_packet + 6], 0x0000
    mov dword [dap_packet + 8], 17
    
    mov ah, 0x42
    mov dl, [drive_number]
    mov si, dap_packet
    int 0x13
    jc disk_error

    ; 2. 解析 BCB 获取 LBA 和 大小
    cmp word [0x4000], 0x4243
    jne .use_default_lba

    mov eax, [0x4004]
    mov dword [dap_packet + 8], eax
    mov ax, [0x4008]
    mov [sectors_left], ax
    jmp .start_kernel_read

.use_default_lba:
    mov dword [dap_packet + 8], 18
    mov word [sectors_left], 128

    ; 3. LBA 循环加载内核到 0x0900:0000
.start_kernel_read:
    mov word [dap_packet + 2], 1
    mov word [dap_packet + 4], 0x0000
    mov word [dap_packet + 6], 0x0900

.lba_kernel_loop:
    mov ah, 0x42
    mov dl, [drive_number]
    mov si, dap_packet
    int 0x13
    jc disk_error

    ; 推进内存地址 (防 64KB 越界)
    add word [dap_packet + 4], 512
    jnc .no_seg_inc
    add word [dap_packet + 6], 0x20
.no_seg_inc:

    ; 推进 LBA 和 循环控制
    inc dword [dap_packet + 8]
    dec word [sectors_left]
    jnz .lba_kernel_loop

    ; 4. 写 boot_info 供 C 代码读取
    xor ax, ax
    mov es, ax
    mov di, 0x0500
    mov al, [drive_number]
    stosb
    mov al, 1
    stosb
    mov ax, [sectors_per_track]
    stosw
    movzx ax, byte [num_heads]
    stosw
    xor eax, eax
    stosd
    mov dword [0x050A], 18
    mov dword [0x050E], 128 * 512
    mov dword [0x0512], 0x9000
    mov dword [0x0516], 0x424F4F54

    cli
    call enable_a20
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode

; ---------------- A20 ----------------
enable_a20:
    mov ax, 0x2401
    int 0x15
    call check_a20
    jnc .done

    in  al, 0x92
    or  al, 2
    out 0x92, al
    call check_a20
    jnc .done

    call a20_kbc
    call check_a20
    jnc .done

    mov si, msg_a20_fail
    call print_string_16
    hlt
.done:
    ret

check_a20:
    pushf
    push ds
    push es
    push di
    push si
    cli
    xor ax, ax
    mov es, ax
    mov di, 0x0500
    not ax
    mov ds, ax
    mov si, 0x0510
    mov al, [es:di]
    push ax
    mov al, [ds:si]
    push ax
    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
    cmp byte [es:di], 0xFF
    pop ax
    mov [ds:si], al
    pop ax
    mov [es:di], al
    mov ax, 0
    je .out
    mov ax, 1
.out:
    pop si
    pop di
    pop es
    pop ds
    popf
    ret

a20_kbc:
    cli
    call .wi
    mov al, 0xAD
    out 0x64, al
    call .wi
    mov al, 0xD0
    out 0x64, al
    call .wo
    in  al, 0x60
    push ax
    call .wi
    mov al, 0xD1
    out 0x64, al
    call .wi
    pop ax
    or  al, 2
    out 0x60, al
    call .wi
    mov al, 0xAE
    out 0x64, al
    call .wi
    sti
    ret
.wi:
    in  al, 0x64
    test al, 2
    jnz .wi
    ret
.wo:
    in  al, 0x64
    test al, 1
    jz .wo
    ret

; ---------------- 32-bit ----------------
[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov byte [0xB8000], '2'
    mov byte [0xB8001], 0x0F
    jmp 0x08:0x9000
    hlt

; ---------------- 16-bit helpers ----------------
[BITS 16]
print_hex:
    pusha
    mov cx, 2
    mov ah, 0x0E
.hex_loop:
    rol al, 4
    mov bl, al
    and bl, 0x0F
    cmp bl, 10
    jl .digit
    add bl, 'A'-10
    jmp .print
.digit:
    add bl, '0'
.print:
    mov al, bl
    int 0x10
    dec cx
    jnz .hex_loop
    popa
    ret

print_string_16:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

disk_error:
    mov [err_code], ah
    mov si, msg_disk_error
    call print_string_16
    mov si, msg_err_code
    call print_string_16
    mov al, [err_code]
    call print_hex
    hlt

; ---------------- Data ----------------
dap_packet:
    db 0x10
    db 0
    dw 1 ;28
    dw 0x0000
    dw 0x0900
    dd 18
    dd 0

msg_loading      db 'Loading OS... ', 0
msg_ok           db 'OK', 0
msg_disk_error   db ' Disk read error! ', 0
msg_err_code     db ' Err:0x', 0
msg_a20_fail     db ' A20 fail!', 0

err_code         db 0
drive_number     db 0
cur_lba          dw 2
cur_cyl          db 0
cur_head         db 0
cur_sector       db 0
sectors_left     dw 64
sectors_per_track db 18
num_heads        db 2

gdt_start:
    dd 0x0
    dd 0x0
    dw 0FFFFh
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
    dw 0FFFFh
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start