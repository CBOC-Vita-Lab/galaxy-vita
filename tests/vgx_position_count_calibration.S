.syntax unified
.text
.thumb
.global _start
.thumb_func
_start:
    bl position_count_begin
    bl _ZN3vgx14DecodeVerticesERKNS_4PlanEPKhjPhPNS_10ArraySpansE
    bl position_count_end
    movs r0, #0
    movs r7, #1
    svc #0

.global position_count_begin
.thumb_func
position_count_begin:
    bx lr
.global position_count_end
.thumb_func
position_count_end:
    bx lr

// The same entry name lets the production trace reader count this known body.
// Eight Thumb slots (one predicate fails), five ARM slots (one fails): 13 / 11.
.global _ZN3vgx14DecodeVerticesERKNS_4PlanEPKhjPhPNS_10ArraySpansE
.thumb_func
_ZN3vgx14DecodeVerticesERKNS_4PlanEPKhjPhPNS_10ArraySpansE:
    push {lr}
    movs r0, #0
    cmp r0, #0
    ite eq
    moveq r0, #7
    movne r0, #9
    blx calibration_arm
    pop {pc}

.arm
.align 2
calibration_arm:
    movs r0, #0
    cmp r0, #0
    moveq r0, #7
    movne r0, #9
    bx lr
.section .note.GNU-stack,"",%progbits
