    .section .rodata
    .p2align 3
.Lsaved_sentinel:
    .dword 0x4123456789abcdef
    .text
    .macro seed_g reg, offset
    st.d \reg, $sp, \offset
    ld.d \reg, $t0, 0
    .endm
    .macro seed_f reg, offset
    fst.d \reg, $sp, \offset
    fld.d \reg, $t0, 0
    .endm
    .macro check_g reg
    bne \reg, $t2, .Lsaved_fail
    .endm
    .macro check_f reg
    fst.d \reg, $sp, 144
    ld.d $t1, $sp, 144
    bne $t1, $t2, .Lsaved_fail
    .endm
    .globl check_saved
    .type check_saved, @function
check_saved:
    addi.d $sp, $sp, -160
    st.d $ra, $sp, 136
    la.local $t0, .Lsaved_sentinel
    seed_g $s0, 0
    seed_g $s1, 8
    seed_g $s2, 16
    seed_g $s3, 24
    seed_g $s4, 32
    seed_g $s5, 40
    seed_g $s6, 48
    seed_g $s7, 56
    seed_g $s8, 64
    seed_f $fs0, 72
    seed_f $fs1, 80
    seed_f $fs2, 88
    seed_f $fs3, 96
    seed_f $fs4, 104
    seed_f $fs5, 112
    seed_f $fs6, 120
    seed_f $fs7, 128
    bl clobber_saved
    la.local $t0, .Lsaved_sentinel
    ld.d $t2, $t0, 0
    check_g $s0
    check_g $s1
    check_g $s2
    check_g $s3
    check_g $s4
    check_g $s5
    check_g $s6
    check_g $s7
    check_g $s8
    check_f $fs0
    check_f $fs1
    check_f $fs2
    check_f $fs3
    check_f $fs4
    check_f $fs5
    check_f $fs6
    check_f $fs7
    addi.w $a0, $zero, 0
    b .Lsaved_restore
.Lsaved_fail:
    addi.w $a0, $zero, 1
.Lsaved_restore:
    ld.d $s0, $sp, 0
    ld.d $s1, $sp, 8
    ld.d $s2, $sp, 16
    ld.d $s3, $sp, 24
    ld.d $s4, $sp, 32
    ld.d $s5, $sp, 40
    ld.d $s6, $sp, 48
    ld.d $s7, $sp, 56
    ld.d $s8, $sp, 64
    fld.d $fs0, $sp, 72
    fld.d $fs1, $sp, 80
    fld.d $fs2, $sp, 88
    fld.d $fs3, $sp, 96
    fld.d $fs4, $sp, 104
    fld.d $fs5, $sp, 112
    fld.d $fs6, $sp, 120
    fld.d $fs7, $sp, 128
    ld.d $ra, $sp, 136
    addi.d $sp, $sp, 160
    jr $ra
    .size check_saved, .-check_saved
