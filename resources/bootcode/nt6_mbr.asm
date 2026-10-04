; -----------------------------------------------------------------------------
; nt6_mbr.asm - bootroll's own NT6-style master boot record.
;
; Semantics (public, documented behaviour of the Windows Vista+ MBR):
;   1. Relocate itself from 0x7C00 to 0x0600.
;   2. Scan the partition table (0x1BE..0x1FD) for an active entry (status 0x80).
;   3. Read the first sector of that partition to 0x7C00 using INT 13h
;      extensions (AH=42h with a disk address packet). Requires extended
;      reads, as reported by AH=41h.
;   4. Validate the 0xAA55 signature and jump to 0x0000:0x7C00 with
;      DL = boot drive and DS:SI -> the active partition entry.
;   5. On any failure, print a short ASCII message via BIOS teletype and halt.
;
; The code occupies the first 440 bytes of the sector. The disk signature and
; partition table area (0x1B8..0x1FD) stay zero here: bootroll always copies
; only the code area onto a target sector and preserves the target's own
; signature/table. The trailing 0xAA55 makes the blob a valid sector when
; exported to a file as-is.
;
; Assemble: nasm -f bin -o nt6_mbr.bin nt6_mbr.asm
; -----------------------------------------------------------------------------
BITS 16
ORG 0x0600

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    cli
    mov ss, ax
    mov sp, 0x7C00              ; stack just below the load address
    sti
    cld
    mov si, 0x7C00              ; relocate 512 bytes down to 0x0600
    mov di, 0x0600
    mov cx, 0x0100
    rep movsw
    jmp 0x0000:relocated

relocated:
    mov [boot_drive], dl        ; BIOS passes the boot drive in DL
    mov si, 0x07BE              ; first partition entry (0x0600 + 0x1BE)
    mov cx, 4

scan_entry:
    cmp byte [si], 0x80         ; active/bootable flag
    je have_entry
    add si, 16
    loop scan_entry
    jmp no_active

have_entry:
    ; Try INT 13h extensions once; the NT6 semantics require them.
    mov dl, [boot_drive]
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc no_extensions

    mov eax, [si + 8]           ; partition start LBA -> DAP
    mov [dap_lba], eax
    xor eax, eax
    mov [dap_lba + 4], eax

    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42                ; extended read -> 0x7C00
    int 0x13
    jc read_error

    cmp word [0x7DFE], 0xAA55   ; partition VBR signature
    jne bad_signature

    mov dl, [boot_drive]        ; DL = drive, DS:SI = active entry (si unchanged
    jmp 0x0000:0x7C00           ; from the scan; DS = 0)

; --- failure paths -----------------------------------------------------------
no_extensions:
    mov si, msg_no_ext
    jmp print_halt
no_active:
    mov si, msg_no_active
    jmp print_halt
read_error:
    mov si, msg_read_error
    jmp print_halt
bad_signature:
    mov si, msg_bad_sig
    ; fall through

print_halt:
    lodsb                       ; print ASCIIZ at DS:SI, then halt
    or al, al
    jz halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_halt
halt:
    hlt
    jmp halt

; --- data --------------------------------------------------------------------
boot_drive: db 0
align 2
dap:                        ; disk address packet (extended read)
    dw 0x0010               ; packet size
    dw 0x0001               ; one sector
    dw 0x7C00               ; transfer buffer offset
    dw 0x0000               ; transfer buffer segment
dap_lba: dq 0               ; start LBA (filled at runtime)

; The classic NT MBR error strings are used deliberately: they are the
; documented on-disk markers that make every tool (bootroll's detector, the
; original BOOTICE, ...) recognize this code as a Windows NT6.x family MBR.
msg_no_ext:     db "Error loading operating system.", 0
msg_no_active:  db "Missing operating system.", 0
msg_read_error: db "Error loading operating system.", 0
msg_bad_sig:    db "Missing operating system.", 0

; --- layout ------------------------------------------------------------------
    times 440 - ($ - $$) db 0   ; code area fills [0x000, 0x1B8)
    ; 0x1B8..0x1BD: disk signature + 0x1BE..0x1FD: partition table are zero;
    ; bootroll patches the target's originals in at install time.
    times 0x1FE - ($ - $$) db 0
    dw 0xAA55                   ; makes the blob a valid standalone sector
