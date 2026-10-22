# Default exception handler of ashexc.prx (see ashexc.c). Runs in exception context (kernel, EXL=1) with the
# faulting thread's GPRs; k0/k1 are free. Layout = AshExcRegs in src/diag/diag.h:
#   0 magic | 4 installed | 8 r[32] (8 + 4*i) | 136 epc | 140 cause | 144 badvaddr | 148 status | 152 lo | 156 hi
	.set noreorder
	.set noat
	.text
	.globl ashexc_handler
	.ent ashexc_handler
ashexc_handler:
	nop                                  # the exception manager wants a NOP first (pspexception.h)
	lui   $k0, %hi(ashexc_cfg)
	addiu $k0, $k0, %lo(ashexc_cfg)
	lw    $k0, 0($k0)                    # user AshExcRegs
1:	beqz  $k0, 1b                        # no block: spin (never happens: cfg is set before registering)
	nop
	sw    $0,   8($k0)
	sw    $1,  12($k0)
	sw    $2,  16($k0)
	sw    $3,  20($k0)
	sw    $4,  24($k0)
	sw    $5,  28($k0)
	sw    $6,  32($k0)
	sw    $7,  36($k0)
	sw    $8,  40($k0)
	sw    $9,  44($k0)
	sw    $10, 48($k0)
	sw    $11, 52($k0)
	sw    $12, 56($k0)
	sw    $13, 60($k0)
	sw    $14, 64($k0)
	sw    $15, 68($k0)
	sw    $16, 72($k0)
	sw    $17, 76($k0)
	sw    $18, 80($k0)
	sw    $19, 84($k0)
	sw    $20, 88($k0)
	sw    $21, 92($k0)
	sw    $22, 96($k0)
	sw    $23, 100($k0)
	sw    $24, 104($k0)
	sw    $25, 108($k0)
	sw    $0,  112($k0)                  # k0/k1: not the thread's
	sw    $0,  116($k0)
	sw    $28, 120($k0)
	sw    $29, 124($k0)
	sw    $30, 128($k0)
	sw    $31, 132($k0)
	mfc0  $k1, $14
	sw    $k1, 136($k0)                  # EPC
	mfc0  $k1, $13
	sw    $k1, 140($k0)                  # Cause
	mfc0  $k1, $8
	sw    $k1, 144($k0)                  # BadVAddr
	mfc0  $k1, $12
	sw    $k1, 148($k0)                  # Status
	mflo  $k1
	sw    $k1, 152($k0)
	mfhi  $k1
	sw    $k1, 156($k0)
	lui   $k1, 0xA5EC
	ori   $k1, $k1, 0x0001
	sw    $k1, 0($k0)                    # magic: block filled
	lui   $k1, %hi(ashexc_cfg)
	addiu $k1, $k1, %lo(ashexc_cfg)
	lw    $sp, 8($k1)                    # spare stack of the game
	lw    $k1, 4($k1)                    # park function of the game
	beqz  $k1, 1b
	nop
	mtc0  $k1, $14                       # EPC = park
	move  $a0, $k0                       # park(AshExcRegs *)
	nop
	nop
	eret
	nop
	.end ashexc_handler
