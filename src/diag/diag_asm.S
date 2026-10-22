# User-mode helpers for src/diag tests (diag_test_crash(1)): a software register snapshot in the AshExcRegs layout of
# exc/ashexc_asm.S, and a jump to the park function on its own stack.
	.set noreorder
	.set noat
	.text
	.globl diag_soft_snapshot
	.ent diag_soft_snapshot
diag_soft_snapshot:                      # a0 = AshExcRegs*
	sw    $0,   8($a0)
	sw    $1,  12($a0)
	sw    $2,  16($a0)
	sw    $3,  20($a0)
	sw    $4,  24($a0)
	sw    $5,  28($a0)
	sw    $6,  32($a0)
	sw    $7,  36($a0)
	sw    $8,  40($a0)
	sw    $9,  44($a0)
	sw    $10, 48($a0)
	sw    $11, 52($a0)
	sw    $12, 56($a0)
	sw    $13, 60($a0)
	sw    $14, 64($a0)
	sw    $15, 68($a0)
	sw    $16, 72($a0)
	sw    $17, 76($a0)
	sw    $18, 80($a0)
	sw    $19, 84($a0)
	sw    $20, 88($a0)
	sw    $21, 92($a0)
	sw    $22, 96($a0)
	sw    $23, 100($a0)
	sw    $24, 104($a0)
	sw    $25, 108($a0)
	sw    $0,  112($a0)
	sw    $0,  116($a0)
	sw    $28, 120($a0)
	sw    $29, 124($a0)
	sw    $30, 128($a0)
	sw    $31, 132($a0)
	addiu $t0, $ra, -8                   # "faulting" pc = the jal that called us
	sw    $t0, 136($a0)
	sw    $0,  148($a0)
	mflo  $t0
	sw    $t0, 152($a0)
	mfhi  $t0
	sw    $t0, 156($a0)
	jr    $ra
	nop
	.end diag_soft_snapshot

	.globl diag_jump_park
	.ent diag_jump_park
diag_jump_park:                          # a0 = regs, a1 = fn, a2 = stack top
	move  $sp, $a2
	move  $t9, $a1
	jr    $t9
	nop
	.end diag_jump_park
