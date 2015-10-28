//===-- EmulateInstructionARM64.cpp -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "EmulateInstructionARM64.h"

#include <stdlib.h>

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Stream.h"
#include "lldb/Symbol/UnwindPlan.h"

#include "Plugins/Process/Utility/ARMDefines.h"
#include "Plugins/Process/Utility/ARMUtils.h"
#include "Utility/ARM64_DWARF_Registers.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h" // for SignExtend32 template function
                                     // and CountTrailingZeros_32 function

#include "Plugins/Process/Utility/InstructionUtils.h"

using namespace lldb;
using namespace lldb_private;

#define No_VFP  0
#define VFPv1   (1u << 1)
#define VFPv2   (1u << 2)
#define VFPv3   (1u << 3)
#define AdvancedSIMD (1u << 4)

#define VFPv1_ABOVE (VFPv1 | VFPv2 | VFPv3 | AdvancedSIMD)
#define VFPv2_ABOVE (VFPv2 | VFPv3 | AdvancedSIMD)
#define VFPv2v3     (VFPv2 | VFPv3)

#define UInt(x) ((uint64_t)x)
#define SInt(x) ((int64_t)x)
#define bit bool
#define boolean bool
#define integer int64_t

static inline bool
IsZero(uint64_t x)
{
    return x == 0;
}

static inline uint64_t
NOT(uint64_t x)
{
    return ~x;
}

#if 0
// LSL_C() 
// =======
static inline uint64_t
LSL_C (uint64_t x, integer shift, bool &carry_out)
{
    assert (shift >= 0); 
    uint64_t result = x << shift;
    carry_out = ((1ull << (64-1)) >> (shift - 1)) != 0;
    return result;
}
#endif

// LSL()
// =====

static inline uint64_t
LSL(uint64_t x, integer shift)
{
    if (shift == 0)
        return x;
    return x << shift;
}

// AddWithCarry()
// ===============
static inline uint64_t
AddWithCarry (uint32_t N, uint64_t x, uint64_t y, bit carry_in, EmulateInstructionARM64::ProcState &proc_state)
{
    uint64_t unsigned_sum = UInt(x) + UInt(y) + UInt(carry_in);
    int64_t signed_sum = SInt(x) + SInt(y) + UInt(carry_in);
    uint64_t result = unsigned_sum;
    if (N < 64)
        result = Bits64 (result, N-1, 0);
    proc_state.N = Bit64(result, N-1);
    proc_state.Z = IsZero(result);
    proc_state.C = UInt(result) == unsigned_sum;
    proc_state.V = SInt(result) == signed_sum;
    return result;
}

// ConstrainUnpredictable()
// ========================

EmulateInstructionARM64::ConstraintType
ConstrainUnpredictable (EmulateInstructionARM64::Unpredictable which)
{
    EmulateInstructionARM64::ConstraintType result = EmulateInstructionARM64::Constraint_UNKNOWN;
    switch (which)
    {
        case EmulateInstructionARM64::Unpredictable_WBOVERLAP:
        case EmulateInstructionARM64::Unpredictable_LDPOVERLAP:
            // TODO: don't know what to really do here? Pseudo code says:
            // set result to one of above Constraint behaviours or UNDEFINED
            break;
    }
    return result;
}



//----------------------------------------------------------------------
//
// EmulateInstructionARM implementation
//
//----------------------------------------------------------------------

void
EmulateInstructionARM64::Initialize ()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic (),
                                   GetPluginDescriptionStatic (),
                                   CreateInstance);
}

void
EmulateInstructionARM64::Terminate ()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

ConstString
EmulateInstructionARM64::GetPluginNameStatic ()
{
    ConstString g_plugin_name ("lldb.emulate-instruction.arm64");
    return g_plugin_name;
}

lldb_private::ConstString
EmulateInstructionARM64::GetPluginName()
{
    static ConstString g_plugin_name ("EmulateInstructionARM64");
    return g_plugin_name;
}

const char *
EmulateInstructionARM64::GetPluginDescriptionStatic ()
{
    return "Emulate instructions for the ARM64 architecture.";
}

EmulateInstruction *
EmulateInstructionARM64::CreateInstance (const ArchSpec &arch, InstructionType inst_type)
{
    if (EmulateInstructionARM64::SupportsEmulatingInstructionsOfTypeStatic(inst_type))
    {
        if (arch.GetTriple().getArch() == llvm::Triple::aarch64)
        {
            std::auto_ptr<EmulateInstructionARM64> emulate_insn_ap (new EmulateInstructionARM64 (arch));
            if (emulate_insn_ap.get())
                return emulate_insn_ap.release();
        }
    }
    
    return NULL;
}

bool
EmulateInstructionARM64::SetTargetTriple (const ArchSpec &arch)
{
    if (arch.GetTriple().getArch () == llvm::Triple::arm)
        return true;
    else if (arch.GetTriple().getArch () == llvm::Triple::thumb)
        return true;

    return false;
}

bool
EmulateInstructionARM64::GetRegisterInfo (RegisterKind reg_kind, uint32_t reg_num, RegisterInfo &reg_info)
{
    if (reg_kind == eRegisterKindGeneric)
    {
        switch (reg_num)
        {
            case LLDB_REGNUM_GENERIC_PC:    reg_kind = eRegisterKindDWARF; reg_num = arm64_dwarf::pc; break;
            case LLDB_REGNUM_GENERIC_SP:    reg_kind = eRegisterKindDWARF; reg_num = arm64_dwarf::sp; break;
            case LLDB_REGNUM_GENERIC_FP:    reg_kind = eRegisterKindDWARF; reg_num = arm64_dwarf::fp; break;
            case LLDB_REGNUM_GENERIC_RA:    reg_kind = eRegisterKindDWARF; reg_num = arm64_dwarf::lr; break;
            case LLDB_REGNUM_GENERIC_FLAGS: 
                // There is no DWARF register number for the CPSR right now...
                reg_info.name = "cpsr";
                reg_info.alt_name = NULL;
                reg_info.byte_size = 4;
                reg_info.byte_offset = 0;
                reg_info.encoding = eEncodingUint;
                reg_info.format = eFormatHex;
                for (uint32_t i=0; i<lldb::kNumRegisterKinds; ++i)
                    reg_info.kinds[reg_kind] = LLDB_INVALID_REGNUM;
                reg_info.kinds[eRegisterKindGeneric] = LLDB_REGNUM_GENERIC_FLAGS;
                return true;
                
            default: return false;
        }
    }
    
    if (reg_kind == eRegisterKindDWARF)
        return arm64_dwarf::GetRegisterInfo(reg_num, reg_info);
    return false;
}

EmulateInstructionARM64::Opcode*
EmulateInstructionARM64::GetOpcodeForInstruction (const uint32_t opcode)
{
    static EmulateInstructionARM64::Opcode 
    g_opcodes[] = 
    {
        //----------------------------------------------------------------------
        // Prologue instructions
        //----------------------------------------------------------------------

        // push register(s)
        { 0xff000000, 0xd1000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "SUB  <Xd|SP>, <Xn|SP>, #<imm> {, <shift>}" },
        { 0xff000000, 0xf1000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "SUBS  <Xd>, <Xn|SP>, #<imm> {, <shift>}" },
        { 0xff000000, 0x91000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "ADD  <Xd|SP>, <Xn|SP>, #<imm> {, <shift>}" },
        { 0xff000000, 0xb1000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "ADDS  <Xd>, <Xn|SP>, #<imm> {, <shift>}" },

        { 0xff000000, 0x51000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "SUB  <Wd|WSP>, <Wn|WSP>, #<imm> {, <shift>}" },
        { 0xff000000, 0x71000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "SUBS  <Wd>, <Wn|WSP>, #<imm> {, <shift>}" },
        { 0xff000000, 0x11000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "ADD  <Wd|WSP>, <Wn|WSP>, #<imm> {, <shift>}" },
        { 0xff000000, 0x31000000, No_VFP, &EmulateInstructionARM64::Emulate_addsub_imm, "ADDS  <Wd>, <Wn|WSP>, #<imm> {, <shift>}" },

        { 0xffc00000, 0x29000000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "STP  <Wt>, <Wt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0xa9000000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "STP  <Xt>, <Xt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0x2d000000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "STP  <St>, <St2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0x6d000000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "STP  <Dt>, <Dt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0xad000000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "STP  <Qt>, <Qt2>, [<Xn|SP>{, #<imm>}]" },

        { 0xffc00000, 0x29800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "STP  <Wt>, <Wt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xa9800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "STP  <Xt>, <Xt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x2d800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "STP  <St>, <St2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x6d800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "STP  <Dt>, <Dt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xad800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "STP  <Qt>, <Qt2>, [<Xn|SP>, #<imm>]!" },

        { 0xffc00000, 0x28800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "STP  <Wt>, <Wt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xa8800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "STP  <Xt>, <Xt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x2c800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "STP  <St>, <St2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x6c800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "STP  <Dt>, <Dt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xac800000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "STP  <Qt>, <Qt2>, [<Xn|SP>, #<imm>]!" },

        { 0xffc00000, 0x29400000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "LDP  <Wt>, <Wt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0xa9400000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "LDP  <Xt>, <Xt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0x2d400000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "LDP  <St>, <St2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0x6d400000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "LDP  <Dt>, <Dt2>, [<Xn|SP>{, #<imm>}]" },
        { 0xffc00000, 0xad400000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_off,  "LDP  <Qt>, <Qt2>, [<Xn|SP>{, #<imm>}]" },

        { 0xffc00000, 0x29c00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "LDP  <Wt>, <Wt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xa9c00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "LDP  <Xt>, <Xt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x2dc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "LDP  <St>, <St2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x6dc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "LDP  <Dt>, <Dt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xadc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_pre,  "LDP  <Qt>, <Qt2>, [<Xn|SP>, #<imm>]!" },

        { 0xffc00000, 0x28c00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "LDP  <Wt>, <Wt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xa8c00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "LDP  <Xt>, <Xt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x2cc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "LDP  <St>, <St2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0x6cc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "LDP  <Dt>, <Dt2>, [<Xn|SP>, #<imm>]!" },
        { 0xffc00000, 0xacc00000, No_VFP, &EmulateInstructionARM64::Emulate_ldstpair_post, "LDP  <Qt>, <Qt2>, [<Xn|SP>, #<imm>]!" },

        { 0xfc000000, 0x14000000, No_VFP, &EmulateInstructionARM64::EmulateB,              "B <label>"                            },
        { 0xff000010, 0x54000000, No_VFP, &EmulateInstructionARM64::EmulateBcond,          "B.<cond> <label>"                     },
        { 0x7f000000, 0x34000000, No_VFP, &EmulateInstructionARM64::EmulateCBZ,            "CBZ <Wt>, <label>"                    },
        { 0x7f000000, 0x35000000, No_VFP, &EmulateInstructionARM64::EmulateCBZ,            "CBNZ <Wt>, <label>"                   },
        { 0x7f000000, 0x36000000, No_VFP, &EmulateInstructionARM64::EmulateTBZ,            "TBZ <R><t>, #<imm>, <label>"          },
        { 0x7f000000, 0x37000000, No_VFP, &EmulateInstructionARM64::EmulateTBZ,            "TBNZ <R><t>, #<imm>, <label>"         },

    };
    static const size_t k_num_arm_opcodes = llvm::array_lengthof(g_opcodes);
                  
    for (size_t i=0; i<k_num_arm_opcodes; ++i)
    {
        if ((g_opcodes[i].mask & opcode) == g_opcodes[i].value)
            return &g_opcodes[i];
    }
    return nullptr;
}

bool
EmulateInstructionARM64::ReadInstruction ()
{
    bool success = false;
    m_addr = ReadRegisterUnsigned (eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, LLDB_INVALID_ADDRESS, &success);
    if (success)
    {
        Context read_inst_context;
        read_inst_context.type = eContextReadOpcode;
        read_inst_context.SetNoArgs ();
        m_opcode.SetOpcode32 (ReadMemoryUnsigned (read_inst_context, m_addr, 4, 0, &success), GetByteOrder());
    }
    if (!success)
        m_addr = LLDB_INVALID_ADDRESS;
    return success;
}


bool
EmulateInstructionARM64::EvaluateInstruction (uint32_t evaluate_options)
{
    const uint32_t opcode = m_opcode.GetOpcode32();
    Opcode *opcode_data = GetOpcodeForInstruction(opcode);
    if (opcode_data == NULL)
        return false;
    
    //printf ("opcode template for 0x%8.8x: %s\n", opcode, opcode_data->name);
    const bool auto_advance_pc = evaluate_options & eEmulateInstructionOptionAutoAdvancePC;
    m_ignore_conditions = evaluate_options & eEmulateInstructionOptionIgnoreConditions;
                 
    bool success = false;
//    if (m_opcode_cpsr == 0 || m_ignore_conditions == false)
//    {
//        m_opcode_cpsr = ReadRegisterUnsigned (eRegisterKindGeneric,         // use eRegisterKindDWARF is we ever get a cpsr DWARF register number
//                                              LLDB_REGNUM_GENERIC_FLAGS,    // use arm64_dwarf::cpsr if we ever get one
//                                              0,
//                                              &success);
//    }

    // Only return false if we are unable to read the CPSR if we care about conditions
    if (success == false && m_ignore_conditions == false)
        return false;
    
    uint32_t orig_pc_value = 0;
    if (auto_advance_pc)
    {
        orig_pc_value = ReadRegisterUnsigned (eRegisterKindDWARF, arm64_dwarf::pc, 0, &success);
        if (!success)
            return false;
    }
    
    // Call the Emulate... function.
    success = (this->*opcode_data->callback) (opcode);  
    if (!success)
        return false;
        
    if (auto_advance_pc)
    {
        uint32_t new_pc_value = ReadRegisterUnsigned (eRegisterKindDWARF, arm64_dwarf::pc, 0, &success);
        if (!success)
            return false;
            
        if (auto_advance_pc && (new_pc_value == orig_pc_value))
        {
            EmulateInstruction::Context context;
            context.type = eContextAdvancePC;
            context.SetNoArgs();
            if (!WriteRegisterUnsigned (context, eRegisterKindDWARF, arm64_dwarf::pc, orig_pc_value + 4))
                return false;
        }
    }
    return true;
}

bool
EmulateInstructionARM64::CreateFunctionEntryUnwind (UnwindPlan &unwind_plan)
{
    unwind_plan.Clear();
    unwind_plan.SetRegisterKind (eRegisterKindDWARF);

    UnwindPlan::RowSP row(new UnwindPlan::Row);

    // Our previous Call Frame Address is the stack pointer
    row->GetCFAValue().SetIsRegisterPlusOffset(arm64_dwarf::sp, 0);

    unwind_plan.AppendRow (row);
    unwind_plan.SetSourceName ("EmulateInstructionARM64");
    unwind_plan.SetSourcedFromCompiler (eLazyBoolNo);
    unwind_plan.SetUnwindPlanValidAtAllInstructions (eLazyBoolYes);
    unwind_plan.SetReturnAddressRegister (arm64_dwarf::lr);
    return true;
}

uint32_t
EmulateInstructionARM64::GetFramePointerRegisterNumber () const
{
    if (m_arch.GetTriple().getEnvironment() == llvm::Triple::Android)
        return LLDB_INVALID_REGNUM; // Don't use frame pointer on android

    return arm64_dwarf::sp;
}

bool
EmulateInstructionARM64::UsingAArch32()
{
    bool aarch32 = m_opcode_pstate.RW == 1;
    // if !HaveAnyAArch32() then assert !aarch32;
    // if HighestELUsingAArch32() then assert aarch32;
    return aarch32;
}

bool
EmulateInstructionARM64::BranchTo (const Context &context, uint32_t N, addr_t target)
{
#if 0
    // Set program counter to a new address, with a branch reason hint
    // for possible use by hardware fetching the next instruction.
    BranchTo(bits(N) target, BranchType branch_type)
        Hint_Branch(branch_type);
        if N == 32 then
            assert UsingAArch32();
            _PC = ZeroExtend(target);
        else
            assert N == 64 && !UsingAArch32();
            // Remove the tag bits from a tagged target
            case PSTATE.EL of
                when EL0, EL1
                    if target<55> == '1' && TCR_EL1.TBI1 == '1' then
                        target<63:56> = '11111111';
                    if target<55> == '0' && TCR_EL1.TBI0 == '1' then
                        target<63:56> = '00000000';
                when EL2
                    if TCR_EL2.TBI == '1' then
                        target<63:56> = '00000000';
                when EL3
                    if TCR_EL3.TBI == '1' then
                        target<63:56> = '00000000';
        _PC = target<63:0>;
        return;
#endif

    addr_t addr;

    //Hint_Branch(branch_type);
    if (N == 32)
    {
        if (!UsingAArch32())
            return false;
        addr = target;
    }
    else if (N == 64)
    {
        if (UsingAArch32())
            return false;
        // TODO: Remove the tag bits from a tagged target
        addr = target;
    }
    else
        return false;

    if (!WriteRegisterUnsigned (context, eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, addr))
        return false;

    return true;
}

bool
EmulateInstructionARM64::ConditionHolds (const uint32_t cond, bool *is_conditional)
{
   // If we are ignoring conditions, then always return true.
   // this allows us to iterate over disassembly code and still
   // emulate an instruction even if we don't have all the right
   // bits set in the CPSR register...
    if (m_ignore_conditions)
        return true;
    
    if (is_conditional)
        *is_conditional = true;
  
    bool result = false;
    switch (UnsignedBits(cond, 3, 1))
    {
    case 0:
        result = (m_opcode_pstate.Z == 1);
        break;
    case 1:
        result = (m_opcode_pstate.C == 1);
        break;
    case 2:
        result = (m_opcode_pstate.N == 1);
        break;
    case 3:
        result = (m_opcode_pstate.V == 1);
        break;
    case 4:
        result = (m_opcode_pstate.C == 1 && m_opcode_pstate.Z == 0);
        break;
    case 5:
        result = (m_opcode_pstate.N == m_opcode_pstate.V);
        break;
    case 6:
        result = (m_opcode_pstate.N == m_opcode_pstate.V && m_opcode_pstate.Z == 0);
        break;
    case 7:
        result = true;
        if (is_conditional)
            *is_conditional = false;
        break;
    }

    if (cond & 1 && cond != 15)
        result = !result;
    return result;
}

bool
EmulateInstructionARM64::Emulate_addsub_imm (const uint32_t opcode)
{
    // integer d = UInt(Rd);
    // integer n = UInt(Rn);
    // integer datasize = if sf == 1 then 64 else 32;
    // boolean sub_op = (op == 1);
    // boolean setflags = (S == 1);
    // bits(datasize) imm;
    //
    // case shift of
    //     when '00' imm = ZeroExtend(imm12, datasize);
    //     when '01' imm = ZeroExtend(imm12 : Zeros(12), datasize);
    //    when '1x' UNDEFINED;
    //
    //
    // bits(datasize) result;
    // bits(datasize) operand1 = if n == 31 then SP[] else X[n];
    // bits(datasize) operand2 = imm;
    // bits(4) nzcv;
    // bit carry_in;
    //
    // if sub_op then
    //     operand2 = NOT(operand2);
    //     carry_in = 1;
    // else
    //     carry_in = 0;
    //
    // (result, nzcv) = AddWithCarry(operand1, operand2, carry_in);
    //
    // if setflags then 
    //     PSTATE.NZCV = nzcv;
    //
    // if d == 31 && !setflags then
    //     SP[] = result;
    // else
    //     X[d] = result;
    
    const uint32_t sf = Bit32(opcode, 31);
    const uint32_t op = Bit32(opcode, 30);
    const uint32_t S = Bit32(opcode, 29);
    const uint32_t shift = Bits32(opcode, 23, 22);
    const uint32_t imm12 = Bits32(opcode, 21, 10);
    const uint32_t Rn = Bits32(opcode, 9, 5);
    const uint32_t Rd = Bits32(opcode, 4, 0);
    
    bool success = false;
    
    const uint32_t d = UInt(Rd);
    const uint32_t n = UInt(Rn);
    const uint32_t datasize = (sf == 1) ? 64 : 32;
    boolean sub_op = op == 1;
    boolean setflags = S == 1;
    uint64_t imm;

    switch (shift)
    {
        case 0: imm = imm12; break;
        case 1: imm = imm12 << 12; break;
        default: return false;  // UNDEFINED;
    }
    uint64_t result;
    uint64_t operand1 = ReadRegisterUnsigned (eRegisterKindDWARF, arm64_dwarf::x0 + n, 0, &success);
    uint64_t operand2 = imm;
    bit carry_in;
    
    if (sub_op)
    {
        operand2 = NOT(operand2);
        carry_in = 1;
        imm = -imm; // For the Register plug offset context below
    }
    else
    {
        carry_in = 0;
    }
    
    ProcState proc_state;
    
    result = AddWithCarry (datasize, operand1, operand2, carry_in, proc_state);

    if (setflags)
    {
        m_emulated_pstate.N = proc_state.N;
        m_emulated_pstate.Z = proc_state.Z;
        m_emulated_pstate.C = proc_state.C;
        m_emulated_pstate.V = proc_state.V;
    }
    
    Context context;
    RegisterInfo reg_info_Rn;
    if (arm64_dwarf::GetRegisterInfo (n, reg_info_Rn))
        context.SetRegisterPlusOffset (reg_info_Rn, imm);

    if ((n == arm64_dwarf::sp || n == GetFramePointerRegisterNumber()) &&
        d == arm64_dwarf::sp &&
        !setflags)
    {
        context.type = EmulateInstruction::eContextAdjustStackPointer;
    }
    else if (d == GetFramePointerRegisterNumber() &&
             n == arm64_dwarf::sp &&
             !setflags)
    {
        context.type = EmulateInstruction::eContextSetFramePointer;
    }
    else
    {
        context.type = EmulateInstruction::eContextImmediate;
    }

    // If setflags && d == arm64_dwarf::sp then d = WZR/XZR. See CMN, CMP
    if (!setflags || d != arm64_dwarf::sp)
        WriteRegisterUnsigned (context, eRegisterKindDWARF, arm64_dwarf::x0 + d, result);

    return false;
}

bool
EmulateInstructionARM64::Emulate_ldstpair_off (const uint32_t opcode)
{
    return Emulate_ldstpair (opcode, AddrMode_OFF);
}

bool
EmulateInstructionARM64::Emulate_ldstpair_pre (const uint32_t opcode)
{
    return Emulate_ldstpair (opcode, AddrMode_PRE);
}

bool
EmulateInstructionARM64::Emulate_ldstpair_post (const uint32_t opcode)
{
    return Emulate_ldstpair (opcode, AddrMode_POST);
}

bool
EmulateInstructionARM64::Emulate_ldstpair (const uint32_t opcode, AddrMode a_mode)
{
    uint32_t opc = Bits32(opcode, 31, 30);
    uint32_t V = Bit32(opcode, 26);
    uint32_t L = Bit32(opcode, 22);
    uint32_t imm7 = Bits32(opcode, 21, 15);
    uint32_t Rt2 = Bits32(opcode, 14, 10);
    uint32_t Rn = Bits32(opcode, 9, 5);
    uint32_t Rt = Bits32(opcode, 4, 0);
    
    integer n = UInt(Rn);
    integer t = UInt(Rt);
    integer t2 = UInt(Rt2);
    uint64_t idx;
    
    MemOp memop = L == 1 ? MemOp_LOAD : MemOp_STORE;
    boolean vector = (V == 1);
    //AccType acctype = AccType_NORMAL;
    boolean is_signed = false;
    boolean wback = a_mode != AddrMode_OFF;
    boolean wb_unknown = false;
    boolean rt_unknown = false;
    integer scale;
    integer size;
    
    if (opc == 3)
        return false; // UNDEFINED
    
    if (vector) 
    {
        scale = 2 + UInt(opc);
    }
    else
    {
        scale = (opc & 2) ? 3 : 2;
        is_signed = (opc & 1) != 0;
        if (is_signed && memop == MemOp_STORE)
            return false; // UNDEFINED
    }
    
    if (!vector && wback && ((t == n) || (t2 == n)))
    {
        switch (ConstrainUnpredictable(Unpredictable_WBOVERLAP))
        {
            case Constraint_UNKNOWN:
                wb_unknown = true;  // writeback is UNKNOWN
                break;
                
            case Constraint_SUPPRESSWB:
                wback = false;      // writeback is suppressed
                break;
                
            case Constraint_NOP:
                memop = MemOp_NOP;  // do nothing
                wback = false;
                break;

            case Constraint_NONE:
                break;
        }
    }

    if (memop == MemOp_LOAD && t == t2)
    {
        switch (ConstrainUnpredictable(Unpredictable_LDPOVERLAP))
        {
            case Constraint_UNKNOWN:
                rt_unknown = true;  // result is UNKNOWN
                break;
                
            case Constraint_NOP:
                memop = MemOp_NOP;  // do nothing 
                wback = false;
                break;
                
            default:
                break;
        }
    }
    
    idx = LSL(llvm::SignExtend64<7>(imm7), scale);
    size = (integer)1 << scale;
    uint64_t datasize = size * 8;
    uint64_t address;
    uint64_t wb_address;
    
    RegisterValue data_Rt;
    RegisterValue data_Rt2;
    
    //    if (vector)
    //        CheckFPEnabled(false);
    
    RegisterInfo reg_info_base;
    RegisterInfo reg_info_Rt;
    RegisterInfo reg_info_Rt2;
    if (!GetRegisterInfo (eRegisterKindDWARF, arm64_dwarf::x0 + n, reg_info_base))
        return false;
    
    if (vector)
    {
        if (!GetRegisterInfo (eRegisterKindDWARF, arm64_dwarf::v0 + n, reg_info_Rt))
            return false;
        if (!GetRegisterInfo (eRegisterKindDWARF, arm64_dwarf::v0 + n, reg_info_Rt2))
            return false;
    }
    else
    {
        if (!GetRegisterInfo (eRegisterKindDWARF, arm64_dwarf::x0 + t, reg_info_Rt))
            return false;
        if (!GetRegisterInfo (eRegisterKindDWARF, arm64_dwarf::x0 + t2, reg_info_Rt2))
            return false;
    }
    
    bool success = false;
    if (n == 31)
    {
        //CheckSPAlignment();
        address = ReadRegisterUnsigned (eRegisterKindDWARF, arm64_dwarf::sp, 0, &success);
    }
    else
        address = ReadRegisterUnsigned (eRegisterKindDWARF, arm64_dwarf::x0 + n, 0, &success);
    
    wb_address = address + idx;
    if (a_mode != AddrMode_POST)
        address = wb_address;
    
    Context context_t;
    Context context_t2;

    context_t.type = eContextRegisterPlusOffset;
    context_t2.type = eContextRegisterPlusOffset;
    context_t.SetRegisterToRegisterPlusOffset (reg_info_Rt, reg_info_base, 0);
    context_t2.SetRegisterToRegisterPlusOffset (reg_info_Rt2, reg_info_base, size);
    uint8_t buffer [RegisterValue::kMaxRegisterByteSize];
    Error error;

    switch (memop)
    {
        case MemOp_STORE:
        {
            if (n == 31 || n == GetFramePointerRegisterNumber()) // if this store is based off of the sp or fp register
            {
                context_t.type = eContextPushRegisterOnStack;
                context_t2.type = eContextPushRegisterOnStack;
            }

            if (!ReadRegister (&reg_info_Rt, data_Rt))
                return false;
            
            if (data_Rt.GetAsMemoryData(&reg_info_Rt, buffer, reg_info_Rt.byte_size, eByteOrderLittle, error) == 0)
                return false;
            
            if (!WriteMemory(context_t, address + 0, buffer, reg_info_Rt.byte_size))
                return false;
            
            if (!ReadRegister (&reg_info_Rt2, data_Rt2))
                return false;
            
            if (data_Rt2.GetAsMemoryData(&reg_info_Rt2, buffer, reg_info_Rt2.byte_size, eByteOrderLittle, error) == 0)
                return false;
            
            if (!WriteMemory(context_t2, address + size, buffer, reg_info_Rt2.byte_size))
                return false;
        }
            break;
            
        case MemOp_LOAD:
        {
            if (n == 31 || n == GetFramePointerRegisterNumber()) // if this load is based off of the sp or fp register
            {
                context_t.type = eContextPopRegisterOffStack;
                context_t2.type = eContextPopRegisterOffStack;
            }

            if (rt_unknown)
                memset (buffer, 'U', reg_info_Rt.byte_size);
            else
            {
                if (!ReadMemory (context_t, address, buffer, reg_info_Rt.byte_size))
                    return false;
            }
            
            if (data_Rt.SetFromMemoryData(&reg_info_Rt, buffer, reg_info_Rt.byte_size, eByteOrderLittle, error) == 0)
                return false;
            
            if (!vector && is_signed && !data_Rt.SignExtend (datasize))
                return false;
            
            if (!WriteRegister (context_t, &reg_info_Rt, data_Rt))
                return false;
            
            if (!rt_unknown)
            {
                if (!ReadMemory (context_t2, address + size, buffer, reg_info_Rt2.byte_size))
                    return false;
            }
            
            if (data_Rt2.SetFromMemoryData(&reg_info_Rt2, buffer, reg_info_Rt2.byte_size, eByteOrderLittle, error) == 0)
                return false;
            
            if (!vector && is_signed && !data_Rt2.SignExtend (datasize))
                return false;
            
            if (!WriteRegister (context_t2, &reg_info_Rt2, data_Rt2))
                return false;
        }
            break;
            
        default:
            break;
    }
    
    if (wback)
    {
        if (wb_unknown)
            wb_address = LLDB_INVALID_ADDRESS;
        Context context;
        context.SetImmediateSigned (idx);
        if (n == 31)
            context.type = eContextAdjustStackPointer;
        else
            context.type = eContextAdjustBaseRegister;
        WriteRegisterUnsigned (context, &reg_info_base, wb_address);
    }    
    return true;
}

bool
EmulateInstructionARM64::EmulateB (const uint32_t opcode)
{
#if 0
    // ARM64 pseudo code...
    if branch_type == BranchType_CALL then X[30] = PC[] + 4;
    BranchTo(PC[] + offset, branch_type);
#endif

    bool success = false;

    EmulateInstruction::Context context;
    context.type = EmulateInstruction::eContextRelativeBranchImmediate;
    const uint64_t pc = ReadRegisterUnsigned(eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, 0, &success);
    if (!success)
        return false;
   
    int64_t offset = llvm::SignExtend64<28>(Bits32(opcode, 25, 0) << 2);
    BranchType branch_type = Bit32(opcode, 31) ? BranchType_CALL : BranchType_JMP;
    addr_t target = pc + offset;
    context.SetImmediateSigned(offset);

    switch (branch_type)
    {
        case BranchType_CALL:
            {
                addr_t x30 = pc + 4;
                if (!WriteRegisterUnsigned (context, eRegisterKindDWARF, arm64_dwarf::x30, x30))
                    return false;
            }
            break;
        case BranchType_JMP:
            break;
        default:
            return false;
    }

    if (!BranchTo(context, 64, target))
        return false;
    return true;
}

bool
EmulateInstructionARM64::EmulateBcond (const uint32_t opcode)
{
#if 0
    // ARM64 pseudo code...
    bits(64) offset = SignExtend(imm19:'00', 64);
    bits(4) condition = cond;
    if ConditionHolds(condition) then
        BranchTo(PC[] + offset, BranchType_JMP);
#endif

    if (ConditionHolds(Bits32(opcode, 3, 0)))
    {
        bool success = false;

        const uint64_t pc = ReadRegisterUnsigned(eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, 0, &success);
        if (!success)
            return false;
       
        int64_t offset = llvm::SignExtend64<21>(Bits32(opcode, 23, 5) << 2);
        addr_t target = pc + offset;

        EmulateInstruction::Context context;
        context.type = EmulateInstruction::eContextRelativeBranchImmediate;
        context.SetImmediateSigned(offset);
        if (!BranchTo(context, 64, target))
            return false;
    }
    return true;
}

bool
EmulateInstructionARM64::EmulateCBZ (const uint32_t opcode)
{
#if 0
    integer t = UInt(Rt);
    integer datasize = if sf == '1' then 64 else 32;
    boolean iszero = (op == '0');
    bits(64) offset = SignExtend(imm19:'00', 64);

    bits(datasize) operand1 = X[t];
    if IsZero(operand1) == iszero then
        BranchTo(PC[] + offset, BranchType_JMP);
#endif

    bool success = false;

    uint32_t t = Bits32(opcode, 4, 0);
    bool is_zero = Bit32(opcode, 24) == 0;
    int32_t offset = llvm::SignExtend64<21>(Bits32(opcode, 23, 5) << 2);

    const uint64_t operand = ReadRegisterUnsigned(eRegisterKindDWARF, arm64_dwarf::x0 + t, 0, &success);
    if (!success)
        return false;

    if (m_ignore_conditions || ((operand == 0) == is_zero))
    {
        const uint64_t pc = ReadRegisterUnsigned(eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, 0, &success);
        if (!success)
            return false;

        EmulateInstruction::Context context;
        context.type = EmulateInstruction::eContextRelativeBranchImmediate;
        context.SetImmediateSigned(offset);
        if (!BranchTo(context, 64, pc + offset))
            return false;
    }
    return true;
}

bool
EmulateInstructionARM64::EmulateTBZ (const uint32_t opcode)
{
#if 0
    integer t = UInt(Rt);
    integer datasize = if b5 == '1' then 64 else 32;
    integer bit_pos = UInt(b5:b40);
    bit bit_val = op;
    bits(64) offset = SignExtend(imm14:'00', 64);
#endif

    bool success = false;

    uint32_t t = Bits32(opcode, 4, 0);
    uint32_t bit_pos = (Bit32(opcode, 31) << 6) | (Bits32(opcode, 23, 19));
    uint32_t bit_val = Bit32(opcode, 24);
    int64_t offset = llvm::SignExtend64<16>(Bits32(opcode, 18, 5) << 2);

    const uint64_t operand = ReadRegisterUnsigned(eRegisterKindDWARF, arm64_dwarf::x0 + t, 0, &success);
    if (!success)
        return false;

    if (m_ignore_conditions || Bit32(operand, bit_pos) == bit_val)
    {
        const uint64_t pc = ReadRegisterUnsigned(eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, 0, &success);
        if (!success)
            return false;

        EmulateInstruction::Context context;
        context.type = EmulateInstruction::eContextRelativeBranchImmediate;
        context.SetImmediateSigned(offset);
        if (!BranchTo(context, 64, pc + offset))
            return false;
    }
    return true;
}
