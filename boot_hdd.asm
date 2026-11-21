[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [drive_number], dl      ; U盘/硬盘 BIOS 给 DL=0x80 或 0x00

    ; 复位磁盘控制器
    xor ah, ah
    mov dl, [drive_number]
    int 0x13
    jc disk_error

    ; ========== 探测扩展读 (LBA) 是否支持 ==========
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive_number]
    int 0x13
    jc  .try_chs               ; 探测失败，直接走 CHS
    cmp bx, 0xAA55
    jne .try_chs               ; 不支持，走 CHS

    ; ========== LBA 路径 ==========
    mov ah, 0x42
    mov dl, [drive_number]
    mov si, dap_packet
    int 0x13
    jnc boot_success           ; 成功，跳转
    jmp .try_chs               ; LBA 失败，尝试 CHS

    ; ========== CHS 回退路径 ==========
.try_chs:
    mov ah, 0x08
    mov dl, [drive_number]
    int 0x13
    jc disk_error
    and cl, 0x3F
    mov [sectors_per_track], cl
    mov [num_heads], dh
    inc byte [num_heads]

    mov word [cur_lba], 1
    mov word [sectors_left], 16
    mov ax, 0x0000
    mov es, ax
    mov bx, 0x7E00

.chs_read_loop:
    movzx ax, byte [num_heads]
    movzx cx, byte [sectors_per_track]
    mul cx
    mov cx, ax

    mov ax, [cur_lba]
    xor dx, dx
    div cx
    mov ch, al
    mov ax, dx

    movzx cx, byte [sectors_per_track]
    xor dx, dx
    div cx
    mov dh, al
    mov cl, dl
    inc cl

    mov ah, 0x02
    mov al, 1
    mov dl, [drive_number]
    int 0x13
    jc disk_error

    add bx, 512
    jnc .no_seg_inc
    mov ax, es
    add ax, 0x20
    mov es, ax
.no_seg_inc:
    inc word [cur_lba]
    dec word [sectors_left]
    jnz .chs_read_loop

boot_success:
    jmp 0x7E00

disk_error:
    mov si, msg_error
.print:
    lodsb
    cmp al, 0
    je .halt
    mov ah, 0x0E
    int 0x10
    jmp .print
.halt:
    hlt
    jmp .halt

; ========== 数据 ==========
drive_number      db 0
cur_lba           dw 1
sectors_left      dw 16
sectors_per_track db 18
num_heads         db 2
msg_error         db 'HDD Boot Error!', 0

dap_packet:
    db 0x10
    db 0
    dw 16
    dw 0x7E00
    dw 0x0000
    dq 1
    dd 0

times 510-($-$$) db 0
dw 0xAA55