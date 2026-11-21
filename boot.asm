[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [drive_number], dl
    mov ah, 0x02
    mov dl, [drive_number]
    mov bx, 0x7E00    
    mov al, 16
    mov ch, 0
    mov cl, 2
    mov dh, 0
    int 0x13
    jc $
    jmp 0x7E00

drive_number  db 0

times 510-($-$$) db 0
dw 0xAA55