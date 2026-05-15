; Copyright (c) 2023-2026 azzuhry (amel111)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
;
; AXAL kernel optimization: Fast sysret path when no task switch occurs.
; sysret is ~5x faster than iretq for returning to userspace.
; We detect task switch by comparing the returned RSP with our saved frame pointer.

global syscall_entry
extern syscall_handler_c

section .text

; Syscall ABI:
; RAX = syscall_num
; RDI = arg1
; RSI = arg2
; RDX = arg3
; R10 = arg4
; R8  = arg5
; R9  = arg6

syscall_entry:
    swapgs 
    
    mov [gs:40], rsp        ; Save user RSP
    mov rsp, [gs:48]        ; Switch to kernel stack

    ; Build iretq frame (needed for slow path and registers_t layout)
    push 0x1B               ; SS (User Data)
    push qword [gs:40]      ; RSP (user)
    push r11                ; RFLAGS (captured by syscall instruction)
    push 0x23               ; CS (User Code)
    push rcx                ; RIP (return address from syscall)
    
    push 0                  ; err_code
    push 0                  ; int_no

    ; Save all registers in registers_t order
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Save SSE/FPU state
    sub rsp, 512
    fxsave [rsp]

    ; Save our frame base for fast-path detection
    mov rbx, rsp           ; rbx = our registers_t frame pointer

    ; Call C handler with registers_t*
    mov rdi, rsp
    call syscall_handler_c

    ; rax = resulting RSP (same as rbx if no task switch, different if switched)
    cmp rax, rbx
    jne .slow_path_iretq   ; Task switch occurred — must use iretq

    ; ===== FAST PATH: sysret (no task switch) =====
    ; Restore FPU state
    fxrstor [rsp]
    add rsp, 512

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                 ; Will be overwritten below with RFLAGS
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx                 ; Will be overwritten below with RIP
    pop rbx
    pop rax                 ; syscall return value is in rax from C handler

    add rsp, 16             ; Skip int_no, err_code

    ; Now at iretq frame: RIP, CS, RFLAGS, RSP, SS
    ; sysret expects: RCX = return RIP, R11 = return RFLAGS
    pop rcx                 ; RIP -> RCX for sysret
    add rsp, 8              ; Skip CS
    pop r11                 ; RFLAGS -> R11 for sysret
    pop rsp                 ; Restore user RSP (skip SS, we know it's 0x1B)

    swapgs
    o64 sysret              ; Fast return to userspace (RCX=RIP, R11=RFLAGS)

.slow_path_iretq:
    ; ===== SLOW PATH: task switch occurred =====
    mov rsp, rax            ; Switch to new task's frame

    ; Restore FPU state
    fxrstor [rsp]
    add rsp, 512

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; Skip int_no, err_code
    
    swapgs 
    iretq

section .bss
