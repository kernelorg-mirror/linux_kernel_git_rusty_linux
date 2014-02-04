/*
 * This file contains the generic code to perform a call to the
 * pSeries LPAR hypervisor.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <asm/hvcall.h>
#include <asm/processor.h>
#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/ptrace.h>
	
#ifdef CONFIG_TRACEPOINTS

	.section	".toc","aw"

	.globl hcall_tracepoint_refcount
hcall_tracepoint_refcount:
	.llong	0

	.section	".text"

/*
 * __trace_hcall_entry and __trace_hcall_exit must balance, so we cache the
 * value of hcall_tracepoint_refcount in the unused STK_GOT slot. We place
 * the hcall opcode in the top 32bits of STK_GOT.
 *
 * If we are tracing we need to create a stack frame to save away all
 * arguments.
 *
 * We branch around the tracing path in early init (eg when populating
 * the MMU hashtable) by using an unconditional cpu feature.
 */
#define HCALL_INST_PRECALL(FIRST_ARG_OFFSET)			\
BEGIN_FTR_SECTION;						\
	li	r12,0;						\
FTR_SECTION_ELSE;						\
	ld	r12,hcall_tracepoint_refcount@toc(r2);		\
ALT_FTR_SECTION_END(0, 1);					\
	std	r12,STK_GOT(r1);				\
	cmpdi	r12,0;						\
	beq+	1f;						\
	sldi	r0,r3,32;					\
	or	r0,r0,r12;					\
	std	r0,STK_GOT(r1);					\
	mflr	r0;						\
	stdu	r1,-STACKFRAMESIZE(r1);				\
	std	r3,STK_REG(R24)(r1);				\
	std	r4,STK_REG(R25)(r1);				\
	std	r5,STK_REG(R26)(r1);				\
	std	r6,STK_REG(R27)(r1);				\
	std	r7,STK_REG(R28)(r1);				\
	std	r8,STK_REG(R29)(r1);				\
	std	r9,STK_REG(R30)(r1);				\
	std	r10,STK_REG(R31)(r1);				\
	std	r0,STACKFRAMESIZE+16(r1);			\
	addi	r4,r1,(STK_REG(R24) + FIRST_ARG_OFFSET);	\
	bl	__trace_hcall_entry;				\
	ld	r3,STK_REG(R24)(r1);				\
	ld	r4,STK_REG(R25)(r1);				\
	ld	r5,STK_REG(R26)(r1);				\
	ld	r6,STK_REG(R27)(r1);				\
	ld	r7,STK_REG(R28)(r1);				\
	ld	r8,STK_REG(R29)(r1);				\
	ld	r9,STK_REG(R30)(r1);				\
	ld	r10,STK_REG(R31)(r1);				\
	ld	r0,STACKFRAMESIZE+16(r1);			\
	addi	r1,r1,STACKFRAMESIZE;				\
	mtlr	r0;						\
1:

/*
 * postcall is performed immediately before function return which
 * allows liberal use of volatile registers.  
 *
 * We branch around the tracing path in early init (eg when populating
 * the MMU hashtable) by using an unconditional cpu feature.
 */
#define __HCALL_INST_POSTCALL					\
BEGIN_FTR_SECTION;						\
	li	r12,0;						\
FTR_SECTION_ELSE;						\
	ld	r12,STK_GOT(r1);				\
ALT_FTR_SECTION_END(0, 1);					\
	cmpdi	r12,0;						\
	beq+	1f;						\
	mflr	r0;						\
	stdu	r1,-STACKFRAMESIZE(r1);				\
	std	r3,STK_REG(R24)(r1);				\
	std	r0,STACKFRAMESIZE+16(r1);			\
	mr	r4,r3;						\
	srdi	r3,r12,32;					\
	bl	__trace_hcall_exit;				\
	ld	r0,STACKFRAMESIZE+16(r1);			\
	ld	r3,STK_REG(R24)(r1);				\
	addi	r1,r1,STACKFRAMESIZE;				\
	mtlr	r0;						\
1:

#define HCALL_INST_POSTCALL_NORETS				\
	li	r5,0;						\
	__HCALL_INST_POSTCALL

#define HCALL_INST_POSTCALL(BUFREG)				\
	mr	r5,BUFREG;					\
	__HCALL_INST_POSTCALL

#else
#define HCALL_INST_PRECALL(FIRST_ARG_OFFSET)
#define HCALL_INST_POSTCALL_NORETS
#define HCALL_INST_POSTCALL(BUFREG)
#endif

	.text

_GLOBAL(plpar_hcall_norets)
	HMT_MEDIUM

	mfcr	r0
	stw	r0,8(r1)

	HCALL_INST_PRECALL(8)

	HVSC				/* invoke the hypervisor */

	HCALL_INST_POSTCALL_NORETS

	lwz	r0,8(r1)
	mtcrf	0xff,r0
	blr				/* return r3 = status */

_GLOBAL(plpar_hcall)
	HMT_MEDIUM

	mfcr	r0
	stw	r0,8(r1)

	HCALL_INST_PRECALL(16)

	std	r4,-8(r1)		/* Save ret buffer */

	mr	r4,r5
	mr	r5,r6
	mr	r6,r7
	mr	r7,r8
	mr	r8,r9
	mr	r9,r10

	HVSC				/* invoke the hypervisor */

	ld	r12,-8(r1)
	std	r4,  0(r12)
	std	r5,  8(r12)
	std	r6, 16(r12)
	std	r7, 24(r12)

	HCALL_INST_POSTCALL(r12)

	lwz	r0,8(r1)
	mtcrf	0xff,r0

	blr				/* return r3 = status */

/*
 * plpar_hcall_raw can be called in real mode. kexec/kdump need some
 * hypervisor calls to be executed in real mode. So plpar_hcall_raw
 * does not access the per cpu hypervisor call statistics variables,
 * since these variables may not be present in the RMO region.
 */
_GLOBAL(plpar_hcall_raw)
	HMT_MEDIUM

	mfcr	r0
	stw	r0,8(r1)

	std	r4,-8(r1)		/* Save ret buffer */

	mr	r4,r5
	mr	r5,r6
	mr	r6,r7
	mr	r7,r8
	mr	r8,r9
	mr	r9,r10

	HVSC				/* invoke the hypervisor */

	ld	r12,-8(r1)
	std	r4,  0(r12)
	std	r5,  8(r12)
	std	r6, 16(r12)
	std	r7, 24(r12)

	lwz	r0,8(r1)
	mtcrf	0xff,r0

	blr				/* return r3 = status */

_GLOBAL(plpar_hcall9)
	HMT_MEDIUM

	mfcr	r0
	stw	r0,8(r1)

	HCALL_INST_PRECALL(16)

	std     r4,STK_PARAM(R4)(r1)     /* Save ret buffer */

	mr	r4,r5
	mr	r5,r6
	mr	r6,r7
	mr	r7,r8
	mr	r8,r9
	mr	r9,r10
	ld	r10,STK_PARAM(R11)(r1)	 /* put arg7 in R10 */
	ld	r11,STK_PARAM(R12)(r1)	 /* put arg8 in R11 */
	ld	r12,STK_PARAM(R13)(r1)    /* put arg9 in R12 */

	HVSC				/* invoke the hypervisor */

	mr	r0,r12
	ld	r12,STK_PARAM(R4)(r1)
	std	r4,  0(r12)
	std	r5,  8(r12)
	std	r6, 16(r12)
	std	r7, 24(r12)
	std	r8, 32(r12)
	std	r9, 40(r12)
	std	r10,48(r12)
	std	r11,56(r12)
	std	r0, 64(r12)

	HCALL_INST_POSTCALL(r12)

	lwz	r0,8(r1)
	mtcrf	0xff,r0

	blr				/* return r3 = status */

/* See plpar_hcall_raw to see why this is needed */
_GLOBAL(plpar_hcall9_raw)
	HMT_MEDIUM

	mfcr	r0
	stw	r0,8(r1)

	std     r4,STK_PARAM(R4)(r1)     /* Save ret buffer */

	mr	r4,r5
	mr	r5,r6
	mr	r6,r7
	mr	r7,r8
	mr	r8,r9
	mr	r9,r10
	ld	r10,STK_PARAM(R11)(r1)	 /* put arg7 in R10 */
	ld	r11,STK_PARAM(R12)(r1)	 /* put arg8 in R11 */
	ld	r12,STK_PARAM(R13)(r1)    /* put arg9 in R12 */

	HVSC				/* invoke the hypervisor */

	mr	r0,r12
	ld	r12,STK_PARAM(R4)(r1)
	std	r4,  0(r12)
	std	r5,  8(r12)
	std	r6, 16(r12)
	std	r7, 24(r12)
	std	r8, 32(r12)
	std	r9, 40(r12)
	std	r10,48(r12)
	std	r11,56(r12)
	std	r0, 64(r12)

	lwz	r0,8(r1)
	mtcrf	0xff,r0

	blr				/* return r3 = status */
