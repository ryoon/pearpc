/*
 *	PearPC
 *	x86asm.cc
 *
 *	Copyright (C) 2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <cstring>
#include <cstdlib>

#include "tools/snprintf.h"
#include "jitc.h"
#include "jitc_asm.h"
#include "x86asm.h"

/*
 *	internal functions
 */

static inline void FASTCALL jitcMapRegister(NativeReg nreg, PPC_Register creg)
{
	gJITC.nativeReg[nreg] = creg;
	gJITC.clientReg[creg] = nreg;
}

static inline void FASTCALL jitcUnmapRegister(NativeReg reg)
{
	gJITC.clientReg[gJITC.nativeReg[reg]] = REG_NO;
	gJITC.nativeReg[reg] = PPC_REG_NO;
}

static inline void FASTCALL jitcLoadRegister(NativeReg nreg, PPC_Register creg)
{
	asmMOVRegDMem(nreg, (uint32)&gCPU+creg);
	jitcMapRegister(nreg, creg);
	gJITC.nativeRegState[nreg] = rsMapped;
}

static inline void FASTCALL jitcStoreRegister(NativeReg nreg, PPC_Register creg)
{
	asmMOVDMemReg((uint32)&gCPU+creg, nreg);
}

static inline void FASTCALL jitcStoreRegisterUndirty(NativeReg nreg, PPC_Register creg)
{
	jitcStoreRegister(nreg, creg);
	gJITC.nativeRegState[nreg] = rsMapped; // no longer dirty
}

static inline PPC_Register FASTCALL jitcGetRegisterMapping(NativeReg reg)
{
	return gJITC.nativeReg[reg];
}

NativeReg FASTCALL jitcGetClientRegisterMapping(PPC_Register creg)
{
	return gJITC.clientReg[creg];
}

static inline void FASTCALL jitcDiscardRegister(NativeReg r)
{
	// FIXME: move to front of the LRU list
	gJITC.nativeRegState[r] = rsUnused;
}

/*
 *	Puts native register to the end of the LRU list
 */
void FASTCALL jitcTouchRegister(NativeReg r)
{
	NativeRegType *reg = gJITC.nativeRegsList[r];
	if (reg->moreRU) {
		// there's a more recently used register
		if (reg->lessRU) {
			reg->lessRU->moreRU = reg->moreRU;
			reg->moreRU->lessRU = reg->lessRU;
		} else {
			// reg was LRUreg
			gJITC.LRUreg = reg->moreRU;
			reg->moreRU->lessRU = NULL;
		}
		reg->moreRU = NULL;
		reg->lessRU = gJITC.MRUreg;
		gJITC.MRUreg->moreRU = reg;
		gJITC.MRUreg = reg;
	}
}

/*
 *	clobbers and moves to end of LRU list
 */
static inline void FASTCALL jitcClobberAndTouchRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		gJITC.nativeRegState[reg] = rsUnused;
		break;
	case rsUnused:;
	}
	jitcTouchRegister(reg);
}

/*
 *	clobbers and moves to front of LRU list
 */
static inline void FASTCALL jitcClobberAndDiscardRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		jitcDiscardRegister(reg);
		break;
	case rsUnused:;
		/*
		 *	Note: it makes no sense to move this register to
		 *	the front of the LRU list here, since only
		 *	other unused register can be before it in the list
		 *
		 *	Note2: it would even be an error to move it here,
		 *	since ESP isn't in the nativeRegsList
		 */
	}
}

void FASTCALL jitcClobberSingleRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		gJITC.nativeRegState[reg] = rsUnused;
		break;
	case rsUnused:;
	}
}

/*
 *	Dirty register.
 *	Does *not* touch register
 *	Will not produce code.
 */
NativeReg FASTCALL jitcDirtyRegister(NativeReg r)
{
	gJITC.nativeRegState[r] = rsDirty;
	return r;
}

NativeReg FASTCALL jitcAllocFixedRegister(NativeReg reg)
{
	jitcClobberAndTouchRegister(reg);
	return reg;
}

/*
 *	Allocates a native register
 *	May produce a store if no registers are avaiable
 */
NativeReg FASTCALL jitcAllocRegister(int options)
{
	NativeReg reg;
	if (options & NATIVE_REG) {
		// allocate fixed register
		reg = (NativeReg)(options & 0xf);
	} else if (options & NATIVE_REG_8) {
		// allocate eax, ecx, edx or ebx
		NativeRegType *rt = gJITC.LRUreg;
		while (rt->reg > EBX) rt = rt->moreRU;
		reg = rt->reg;
	} else {
		// allocate random register
		reg = gJITC.LRUreg->reg;
	}
	return jitcAllocFixedRegister(reg);
}

/*
 *	Returns native registers that contains value of 
 *	client register or allocates new register which
 *	maps to the client register.
 *	Dirties register.
 *
 *	May produce a store if no registers are avaiable
 *	May produce a MOV/XCHG to satisfy mapping
 *	Will never produce a load
 */
NativeReg FASTCALL jitcMapClientRegisterDirty(PPC_Register creg, int options)
{
	if (options & NATIVE_REG_8) {
		// nyi
		ht_printf("unimpl x86asm:%d\n", __LINE__);
		exit(-1);
	}
	if (options & NATIVE_REG) {
		NativeReg want_reg = (NativeReg)(options & 0xf);
		PPC_Register have_mapping = jitcGetRegisterMapping(want_reg);

		if (have_mapping != PPC_REG_NO) {
			// test if we're lucky
			if (have_mapping == creg) {
				jitcDirtyRegister(want_reg);
				jitcTouchRegister(want_reg);
				return want_reg;
			}

			// we're not lucky, get a new register for the old mapping
			NativeReg temp_reg = jitcAllocRegister();
			// note that AllocRegister also touches temp_reg

			// make new mapping
			jitcMapRegister(want_reg, creg);

			gJITC.nativeRegState[temp_reg] = gJITC.nativeRegState[want_reg];
			// now we can mess with want_reg
			jitcDirtyRegister(want_reg);

			// maybe the old mapping was discarded and we're done
			if (temp_reg == want_reg) return want_reg;

			// ok, restore old mapping
			if (temp_reg == EAX || want_reg == EAX) {
				asmALURegReg(X86_XCHG, temp_reg, want_reg);
			} else {
				asmALURegReg(X86_MOV, temp_reg, want_reg);				
			}
			jitcMapRegister(temp_reg, have_mapping);
		} else {
			// want_reg is free
			// unmap creg if needed
			NativeReg reg = jitcGetClientRegisterMapping(creg);
			if (reg != REG_NO) {
				jitcUnmapRegister(reg);
				jitcDiscardRegister(reg);
			}
			jitcMapRegister(want_reg, creg);
			jitcDirtyRegister(want_reg);
		}
		jitcTouchRegister(want_reg);
		return want_reg;
	} else {
		NativeReg reg = jitcGetClientRegisterMapping(creg);
		if (reg == REG_NO) {
			reg = jitcAllocRegister();
			jitcMapRegister(reg, creg);
		} else {
			jitcTouchRegister(reg);
		}
		return jitcDirtyRegister(reg);
	}
}

 
/*
 *	Returns native registers that contains value of 
 *	client register or allocates new register with
 *	this content.
 *
 *	May produce a store if no registers are avaiable
 *	May produce a load if client registers isn't mapped
 *	May produce a MOV/XCHG to satisfy mapping
 */
NativeReg FASTCALL jitcGetClientRegister(PPC_Register creg, int options)
{
	if (options & NATIVE_REG_8) {
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (client_reg_maps_to == REG_NO) {
			NativeReg reg = jitcAllocRegister(NATIVE_REG_8);
			jitcLoadRegister(reg, creg);
			return reg;
		} else {
			if (client_reg_maps_to <= EBX) {
				jitcTouchRegister(client_reg_maps_to);
				return client_reg_maps_to;
			}
			NativeReg want_reg = jitcAllocRegister(NATIVE_REG_8);
			asmALURegReg(X86_MOV, want_reg, client_reg_maps_to);
			jitcUnmapRegister(client_reg_maps_to);
			jitcMapRegister(want_reg, creg);
			gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
			gJITC.nativeRegState[client_reg_maps_to] = rsUnused;
			return want_reg;
		}
	}
	if (options & NATIVE_REG) {
		NativeReg want_reg = (NativeReg)(options & 0xf);
		PPC_Register native_reg_maps_to = jitcGetRegisterMapping(want_reg);
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (native_reg_maps_to != PPC_REG_NO) {
			// test if we're lucky
			if (native_reg_maps_to == creg) {
				jitcTouchRegister(want_reg);
			} else {
				// we need to satisfy mapping
				if (client_reg_maps_to != REG_NO) {
					asmALURegReg(X86_XCHG, want_reg, client_reg_maps_to);
					RegisterState rs = gJITC.nativeRegState[want_reg];
					gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
					gJITC.nativeRegState[client_reg_maps_to] = rs;
					jitcMapRegister(want_reg, creg);
					jitcMapRegister(client_reg_maps_to, native_reg_maps_to);
					jitcTouchRegister(want_reg);
				} else {
					// client register isn't mapped
					jitcAllocFixedRegister(want_reg);
					jitcLoadRegister(want_reg, creg);
				}
			}
			return want_reg;
		} else {
			// want_reg is free 
			jitcTouchRegister(want_reg);
			if (client_reg_maps_to != REG_NO) {
				asmALURegReg(X86_MOV, want_reg, client_reg_maps_to);
				gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
				jitcUnmapRegister(client_reg_maps_to);
				jitcDiscardRegister(client_reg_maps_to);
				jitcMapRegister(want_reg, creg);
			} else {
				jitcLoadRegister(want_reg, creg);
			}
			return want_reg;
		}
	} else {
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (client_reg_maps_to != REG_NO) {
			jitcTouchRegister(client_reg_maps_to);
			return client_reg_maps_to;
		} else {
			NativeReg reg = jitcAllocRegister();
			jitcLoadRegister(reg, creg);
			return reg;
		}
	}
}

/*
 *	Same as jitcGetClientRegister() but also dirties result
 */
NativeReg FASTCALL jitcGetClientRegisterDirty(PPC_Register creg, int options)
{
	return jitcDirtyRegister(jitcGetClientRegister(creg, options));
}

static inline void FASTCALL jitcFlushSingleRegister(NativeReg reg)
{
	if (gJITC.nativeRegState[reg] == rsDirty) {
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
	}
}

static inline void FASTCALL jitcFlushSingleRegisterDirty(NativeReg reg)
{
	if (gJITC.nativeRegState[reg] == rsDirty) {
		jitcStoreRegister(reg, jitcGetRegisterMapping(reg));
	}
}

/*
 *	Flushes native register(s).
 *	Resets dirty flags.
 *	Will produce a store if register is dirty.
 */
void FASTCALL jitcFlushRegister(int options)
{
	if (options == NATIVE_REGS_ALL) {
		for (NativeReg i = EAX; i <= EDI; i = (NativeReg)(i+1)) jitcFlushSingleRegister(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcFlushSingleRegister(reg);
	}
}

/*
 *	Flushes native register(s).
 *	Doesnt reset dirty flags.
 *	Will produce a store if register is dirty.
 */
void FASTCALL jitcFlushRegisterDirty(int options)
{
	if (options == NATIVE_REGS_ALL) {
		for (NativeReg i = EAX; i <= EDI; i = (NativeReg)(i+1)) jitcFlushSingleRegisterDirty(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcFlushSingleRegisterDirty(reg);
	}
}
/*
 *	Clobbers native register(s).
 *	Register is unused afterwards.
 *	Will produce a store if register was dirty.
 */          
void FASTCALL jitcClobberRegister(int options)
{
	if (options == NATIVE_REGS_ALL) {
		/*
		 *	We dont use clobberAndDiscard here
		 *	since it make no sense to move one register
		 *	if we clobber all
		 */
		for (NativeReg i = EAX; i <= EDI; i=(NativeReg)(i+1)) jitcClobberSingleRegister(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcClobberAndDiscardRegister(reg);
	}
}

/*
 *
 */
void FASTCALL jitcFlushAll()
{
	jitcClobberCarryAndFlags();
	jitcFlushRegister();
}

/*
 *
 */
void FASTCALL jitcClobberAll()
{
	jitcClobberCarryAndFlags();
	jitcClobberRegister();
}

/*
 *	Invalidates all mappings
 *
 *	Will never produce code
 */          
void FASTCALL jitcInvalidateAll()
{
	memset(gJITC.nativeReg, PPC_REG_NO, sizeof gJITC.nativeReg);
	memset(gJITC.nativeRegState, rsUnused, sizeof gJITC.nativeRegState);
	memset(gJITC.clientReg, REG_NO, sizeof gJITC.clientReg);
	gJITC.nativeCarryState = gJITC.nativeFlagsState = rsUnused;
}

/*
 *	Gets the client carry flags into the native carry flag
 *
 *	
 */
void FASTCALL jitcGetClientCarry()
{
	if (gJITC.nativeCarryState == rsUnused) {
		jitcClobberFlags();
		// bt [gCPU.xer], XER_CA
		byte modrm[6];
		asmBTxMemImm(X86_BT, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer), 29);
		gJITC.nativeCarryState = rsMapped;
	}
}

void FASTCALL jitcMapFlagsDirty(PPC_CRx cr)
{
	gJITC.nativeFlags = cr;
	gJITC.nativeFlagsState = rsDirty;
}

PPC_CRx FASTCALL jitcGetFlagsMapping()
{
	return gJITC.nativeFlags;
}

bool FASTCALL jitcFlagsMapped()
{
	return gJITC.nativeFlagsState != rsUnused;
}

bool FASTCALL jitcCarryMapped()
{
	return gJITC.nativeCarryState != rsUnused;
}

void FASTCALL jitcMapCarryDirty()
{
	gJITC.nativeCarryState = rsDirty;
}

void FASTCALL jitcClobberFlags()
{
	if (gJITC.nativeFlagsState == rsDirty) {
		if (gJITC.nativeCarryState == rsDirty) {
			asmCALL((NativeAddress)ppc_flush_carry_and_flags_asm);
		} else {
			asmCALL((NativeAddress)ppc_flush_flags_asm);
		}
		gJITC.nativeCarryState = rsUnused;
	}
	gJITC.nativeFlagsState = rsUnused;
}

void FASTCALL jitcClobberCarry()
{
	if (gJITC.nativeCarryState == rsDirty) {
		if (gJITC.nativeFlagsState == rsDirty) {
			asmCALL((NativeAddress)ppc_flush_carry_and_flags_asm);
		} else {
			// can someone please optimize this??
			NativeAddress fixup = asmJxxFixup(X86_C);
			byte modrm[6];
			asmALUMemImm8(X86_AND, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer+3), ~(1<<5));
			NativeAddress fixup2 = asmJMPFixup();
			asmResolveFixup(fixup, asmHERE());
			asmALUMemImm8(X86_OR, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer+3), 1<<5);
			asmResolveFixup(fixup2, asmHERE());
		}
		gJITC.nativeFlagsState = rsUnused;
	}
	gJITC.nativeCarryState = rsUnused;
}

void FASTCALL jitcClobberCarryAndFlags()
{
	if (gJITC.nativeCarryState == rsDirty) {
		if (gJITC.nativeFlagsState == rsDirty) {
			asmCALL((NativeAddress)ppc_flush_carry_and_flags_asm);
			gJITC.nativeCarryState = gJITC.nativeFlagsState = rsUnused;
		} else {
			jitcClobberCarry();
		}
	} else {
		jitcClobberFlags();
	}
}

/*
 *	ONLY FOR DEBUG! DON'T CALL (unless you know what you are doing)
 */
void FASTCALL jitcFlushCarryAndFlagsDirty()
{
	if (gJITC.nativeCarryState == rsDirty) {
		if (gJITC.nativeFlagsState == rsDirty) {
			asmCALL((NativeAddress)ppc_flush_carry_and_flags_asm);
		} else {
			NativeAddress fixup = asmJxxFixup(X86_C);
			byte modrm[6];
			asmALUMemImm8(X86_AND, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer+3), ~(1<<5));
			NativeAddress fixup2 = asmJMPFixup();
			asmResolveFixup(fixup, asmHERE());
			asmALUMemImm8(X86_OR, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer+3), 1<<5);
			asmResolveFixup(fixup2, asmHERE());
		}
	} else {
		if (gJITC.nativeFlagsState == rsDirty) {
			asmCALL((NativeAddress)ppc_flush_flags_asm);
		}
	}
}

/*
 *
 */

NativeAddress FASTCALL asmHERE()
{
	return gJITC.currentPage->tcp;
}

void FASTCALL asmSimpleMODRMRegReg(uint8 opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[2] = {opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmSimpleMODRMRegReg8(uint8 opc, NativeReg8 reg1, NativeReg8 reg2)
{
	byte instr[2] = {opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmTESTRegImm(NativeReg reg1, uint32 imm)
{
	if (reg1 <= EBX) {
		if (imm <= 0xff) {
			// test al, 1
			if (reg1 == EAX) {
				byte instr[2] = {0xa8, imm};
				jitcEmit(instr, sizeof(instr));
			} else {
				byte instr[3] = {0xf6, 0xc0+reg1, imm};
				jitcEmit(instr, sizeof(instr));
			}
			return;
		} else if (!(imm & 0xffff00ff)) {
			// test ah, 1
			byte instr[3] = {0xf6, 0xc4+reg1, (imm>>8)};
			jitcEmit(instr, sizeof(instr));
			return;
		}
	}
	// test eax, 1001
	if (reg1 == EAX) {
		byte instr[5];
		instr[0] = 0xa9;
		*((uint32 *)&instr[1]) = imm;
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[6];
		instr[0] = 0xf7;
		instr[1] = 0xc0+reg1;
		*((uint32 *)&instr[2]) = imm;
		jitcEmit(instr, sizeof(instr));
	}
}

void FASTCALL asmSimpleALURegReg(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[2] = {0x03+(opc<<3), 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmSimpleALURegReg8(X86ALUopc opc, NativeReg8 reg1, NativeReg8 reg2)
{
	byte instr[2] = {0x02+(opc<<3), 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmALURegReg(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	switch (opc) {
	case X86_MOV: 
		asmSimpleMODRMRegReg(0x8b, reg1, reg2);
	        break;
	case X86_TEST:
		asmSimpleMODRMRegReg(0x85, reg1, reg2);
	        break;
	case X86_XCHG:
		if (reg1 == EAX) {
			jitcEmit1(0x90+reg2);
		} else if (reg2 == EAX) {
			jitcEmit1(0x90+reg1);
		} else {
			asmSimpleMODRMRegReg(0x87, reg1, reg2);
		}
	        break;
	default:
		asmSimpleALURegReg(opc, reg1, reg2);
	}	
}

void FASTCALL asmALURegReg8(X86ALUopc opc, NativeReg8 reg1, NativeReg8 reg2)
{
	switch (opc) {
	case X86_MOV: 
		asmSimpleMODRMRegReg8(0x8a, reg1, reg2);
	        break;
	case X86_TEST:
		asmSimpleMODRMRegReg8(0x84, reg1, reg2);
	        break;
	case X86_XCHG:
		asmSimpleMODRMRegReg8(0x86, reg1, reg2);
	        break;
	default:
		asmSimpleALURegReg8(opc, reg1, reg2);
	}	
}

void FASTCALL asmSimpleALURegImm(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	if (imm <= 0x7f || imm >= 0xffffff80) {
		byte instr[3] = {0x83, 0xc0+(opc<<3)+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	} else {
		if (reg1 == EAX) {
			byte instr[5];
			instr[0] = 0x05+(opc<<3);
			*((uint32 *)&instr[1]) = imm;
			jitcEmit(instr, sizeof(instr));
		} else {
			byte instr[6];
			instr[0] = 0x81;
			instr[1] = 0xc0+(opc<<3)+reg1;
			*((uint32 *)&instr[2]) = imm;
			jitcEmit(instr, sizeof(instr));
		}
	}
}

void FASTCALL asmALURegImm(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	switch (opc) {
	case X86_MOV:
		if (imm == 0) {
			asmALURegReg(X86_XOR, reg1, reg1);
		} else {
			asmMOVRegImm_NoFlags(reg1, imm);
		}
		break;
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		asmTESTRegImm(reg1, imm);
		break;
	case X86_CMP:
//		if (imm == 0) {
//			asmALURegReg(X86_OR, reg1, reg1);
//		} else {
			asmSimpleALURegImm(opc, reg1, imm);
//		}
		break;
	default:
		asmSimpleALURegImm(opc, reg1, imm);
	}
}

void FASTCALL asmMOVRegImm_NoFlags(NativeReg reg1, uint32 imm)
{
	byte instr[5];
	instr[0] = 0xb8+reg1;
	*((uint32 *)&instr[1]) = imm;
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmALUReg(X86ALUopc1 opc, NativeReg reg1)
{
	byte instr[2];
	switch (opc) {
	case X86_NOT:
		instr[0] = 0xf7;
		instr[1] = 0xd0+reg1;
		break;
	case X86_NEG:
		instr[0] = 0xf7;
		instr[1] = 0xd8+reg1;
		break;
	}
	jitcEmit(instr, 2);
}

void FASTCALL asmALUMemReg(X86ALUopc opc, byte *modrm, int len, NativeReg reg2)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x89;
		break;
	case X86_XCHG:
		instr[0] = 0x87;
		break;
	case X86_TEST:
		instr[0] = 0x85;
		break;
	default:
		instr[0] = 0x01+(opc<<3);
	}
	memmove(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}

void FASTCALL asmSimpleALUMemImm(X86ALUopc opc, byte *modrm, int len, uint32 imm)
{
	byte instr[15];
	if (imm <= 0x7f || imm >= 0xffffff80) {
		instr[0] = 0x83;
		memmove(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		instr[len+1] = imm;
		jitcEmit(instr, len+2);
	} else {
		instr[0] = 0x81;
		memmove(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
	}
}

void FASTCALL asmALUMemImm(X86ALUopc opc, byte *modrm, int len, uint32 imm)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV: {
		instr[0] = 0xc7;
		memmove(&instr[1], modrm, len);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
		break;
	}
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		instr[0] = 0xf7;
		memmove(&instr[1], modrm, len);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
		break;
	default:
		asmSimpleALUMemImm(opc, modrm, len, imm);
	}
}

void FASTCALL asmALURegMem(X86ALUopc opc, NativeReg reg1, byte *modrm, int len)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x8b;
		break;
	case X86_XCHG:
		// XCHG is symmetric
		instr[0] = 0x87;
		break;
	case X86_TEST:
		// TEST is symmetric
		instr[0] = 0x85;
		break;
	default:
		instr[0] = 0x03+(opc<<3);
	}
	memmove(&instr[1], modrm, len);
	instr[1] |= (reg1<<3);
	jitcEmit(instr, len+1);
}

void FASTCALL asmALURegMem8(X86ALUopc opc, byte *modrm, int len, NativeReg reg2)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x8a;
		break;
	case X86_XCHG:
		// XCHG is symmetric
		instr[0] = 0x86;
		break;
	case X86_TEST:
		// TEST is symmetric
		instr[0] = 0x84;
		break;
	default:
		instr[0] = 0x02+(opc<<3);
	}
	memmove(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}

void FASTCALL asmALUMemReg8(X86ALUopc opc, byte *modrm, int len, NativeReg reg2)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x88;
		break;
	case X86_XCHG:
		instr[0] = 0x86;
		break;
	case X86_TEST:
		instr[0] = 0x84;
		break;
	default:
		instr[0] = 0x00+(opc<<3);
	}
	memmove(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}

void FASTCALL asmALUMemImm8(X86ALUopc opc, byte *modrm, int len, uint8 imm)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0xc6;
		break;
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		instr[0] = 0xf6;
		break;
	default:
		instr[0] = 0x80;
		memmove(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		instr[len+1] = imm;
		jitcEmit(instr, len+2);
		return;
	}
	memmove(&instr[1], modrm, len);
	instr[len+1] = imm;
	jitcEmit(instr, len+2);
}

void FASTCALL asmMOVDMemReg(uint32 disp, NativeReg reg1)
{
	byte instr[6];
	if (reg1==EAX) {
		instr[0] = 0xa3;
		*((uint32 *)&instr[1]) = disp;
		jitcEmit(instr, 5);
	} else {
		instr[0] = 0x89;
		instr[1] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[2]) = disp;
		jitcEmit(instr, 6);
	}
}

void FASTCALL asmMOVRegDMem(NativeReg reg1, uint32 disp)
{
	byte instr[6];
	if (reg1==EAX) {
		instr[0] = 0xa1;
		*((uint32 *)&instr[1]) = disp;
		jitcEmit(instr, 5);
	} else {
		instr[0] = 0x8b;
		instr[1] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[2]) = disp;
		jitcEmit(instr, 6);
	}
}

void FASTCALL asmTESTDMemImm(uint32 disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x05;
	if (!(imm & 0xffffff00)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = disp;
		instr[6] = imm;
	} else if (!(imm & 0xffff00ff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = disp+1;
		instr[6] = imm >> 8;
	} else if (!(imm & 0xff00ffff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = disp+2;
		instr[6] = imm >> 16;
	} else if (!(imm & 0x00ffffff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0xf7;
		*((uint32 *)&instr[2]) = disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}

void FASTCALL asmANDDMemImm(uint32 disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x25;
	if ((imm & 0xffffff00)==0xffffff00) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp;
		instr[6] = imm;
	} else if ((imm & 0xffff00ff)==0xffff00ff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+1;
		instr[6] = imm >> 8;
	} else if ((imm & 0xff00ffff)==0xff00ffff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+2;
		instr[6] = imm >> 16;
	} else if ((imm & 0x00ffffff)==0x00ffffff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0x81;
		*((uint32 *)&instr[2]) = disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}

void FASTCALL asmORDMemImm(uint32 disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x0d;
	if (!(imm & 0xffffff00)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp;
		instr[6] = imm;
	} else if (!(imm & 0xffff00ff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+1;
		instr[6] = imm >> 8;
	} else if (!(imm & 0xff00ffff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+2;
		instr[6] = imm >> 16;
	} else if (!(imm & 0x00ffffff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0x81;
		*((uint32 *)&instr[2]) = disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}


void FASTCALL asmMOVxxRegReg8(X86MOVxx opc, NativeReg reg1, NativeReg8 reg2)
{
	byte instr[3] = {0x0f, opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmSETReg(X86FlagTest flags, NativeReg reg1)
{
	byte instr[3] = {0x0f, 0x90+flags, 0xc0+reg1};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmCMOVRegReg(X86FlagTest flags, NativeReg reg1, NativeReg reg2)
{
	byte instr[3] = {0x0f, 0x40+flags, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmShiftRegImm(X86ShiftOpc opc, NativeReg reg1, uint32 imm)
{
	if (imm == 1) {
		byte instr[2] = {0xd1, 0xc0+opc+reg1};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[3] = {0xc1, 0xc0+opc+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	}
}

void FASTCALL asmShiftRegCL(X86ShiftOpc opc, NativeReg reg1)
{
	// 0xd3 [ModR/M]
	byte instr[2] = {0xd3, 0xc0+opc+reg1};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmIMULRegRegImm(NativeReg reg1, NativeReg reg2, uint32 imm)
{
	if (imm <= 0x7f || imm >= 0xffffff80) {	
		byte instr[3] = {0x6b, 0xc0+(reg1<<3)+reg2, imm};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[6] = {0x69, 0xc0+(reg1<<3)+reg2};
		*((uint32*)(&instr[2])) = imm;
		jitcEmit(instr, sizeof(instr));
	}
}

void FASTCALL asmIMULRegReg(NativeReg reg1, NativeReg reg2)
{
	byte instr[3] = {0x0f, 0xaf, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmINCReg(NativeReg reg1)
{
	jitcEmit1(0x40+reg1);
}

void FASTCALL asmDECReg(NativeReg reg1)
{
	jitcEmit1(0x48+reg1);
}

void FASTCALL asmLEA(NativeReg reg1, byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0x8d;
	memmove(instr+1, modrm, len);
	instr[1] |= reg1<<3;
	jitcEmit(instr, len+1);
}

void FASTCALL asmBTxMemImm(X86BitTest opc, byte *modrm, int len, int value)
{
	byte instr[15];
	instr[0] = 0x0f;
	instr[1] = 0xba;
	memmove(instr+2, modrm, len);
	instr[2] |= opc<<3;
	instr[len+2] = value;
	jitcEmit(instr, len+3);
}

void FASTCALL asmBSxRegReg(X86BitSearch opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[3] = {0x0f, opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmBSWAP(NativeReg reg)
{
	byte instr[2];
	instr[0] = 0x0f;
	instr[1] = 0xc8+reg;
	jitcEmit(instr, sizeof(instr));	
}

void FASTCALL asmJMP(NativeAddress to)
{
	/*
	 *	We use jitcEmitAssure here, since
	 *	we have to know the exact address of the jump
	 *	instruction (since it is relative)
	 */
restart:
	byte instr[5];
	uint32 rel = (uint32)(to - (gJITC.currentPage->tcp+2));
	if (rel <= 0x7f || rel >= 0xffffff80) {	
		if (!jitcEmitAssure(2)) goto restart;
		instr[0] = 0xeb;
		instr[1] = rel;
		jitcEmit(instr, 2);
	} else {
		if (!jitcEmitAssure(5)) goto restart;
		instr[0] = 0xe9;
		*((uint32 *)&instr[1]) = (uint32)(to - (gJITC.currentPage->tcp+5));
//		*((uint32 *)&instr[1]) = rel - 3;
		jitcEmit(instr, 5);
	}
}

void FASTCALL asmJxx(X86FlagTest flags, NativeAddress to)
{
restart:
	byte instr[6];
	uint32 rel = (uint32)(to - (gJITC.currentPage->tcp+2));
	if (rel <= 0x7f || rel >= 0xffffff80) {
		if (!jitcEmitAssure(2)) goto restart;
		instr[0] = 0x70+flags;
		instr[1] = rel;
		jitcEmit(instr, 2);
	} else {
		if (!jitcEmitAssure(6)) goto restart;
		instr[0] = 0x0f;
		instr[1] = 0x80+flags;
		*((uint32 *)&instr[2]) = (uint32)(to - (gJITC.currentPage->tcp+6));
//		*((uint32 *)&instr[2]) = rel - 3;
		jitcEmit(instr, 6);
	}
}

NativeAddress FASTCALL asmJMPFixup()
{
	byte instr[5];
	instr[0] = 0xe9;
	jitcEmit(instr, 5);
	return gJITC.currentPage->tcp - 4;
}

NativeAddress FASTCALL asmJxxFixup(X86FlagTest flags)
{
	byte instr[6];
	instr[0] = 0x0f;
	instr[1] = 0x80+flags;
	jitcEmit(instr, 6);	
	return gJITC.currentPage->tcp - 4;
}

void FASTCALL asmResolveFixup(NativeAddress at, NativeAddress to)
{
	/*
	 *	yes, I also didn't believe this could be real code until
	 *	I had written it.
	 */
	*((uint32 *)at) = (uint32)(to - ((uint32)at+4));
}

void FASTCALL asmCALL(NativeAddress to)
{
	jitcEmitAssure(5);
	byte instr[5];
	instr[0] = 0xe8;
	*((uint32 *)&instr[1]) = (uint32)(to - (gJITC.currentPage->tcp+5));
	jitcEmit(instr, 5);
}

void FASTCALL asmSimple(X86SimpleOpc simple)
{
	if (simple > 0xff) {
		jitcEmit((byte*)&simple, 2);
	} else {
		jitcEmit1(simple);
	}
}