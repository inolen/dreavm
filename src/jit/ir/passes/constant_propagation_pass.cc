#include <type_traits>
#include <unordered_map>
#include "emu/profiler.h"
#include "jit/ir/passes/constant_propagation_pass.h"

using namespace re::jit::ir;
using namespace re::jit::ir::passes;

typedef void (*FoldFn)(IRBuilder &, Block *, Instr *i);

// specify which arguments must be constant in order for fold operation to run
enum {
  ARG0_CNST = 0x1,
  ARG1_CNST = 0x2,
  ARG2_CNST = 0x4,
};

// fold callbacks for each operaton
std::unordered_map<int, FoldFn> fold_cbs;
int fold_masks[NUM_OPS];

// OP_SELECT and OP_BRANCH_COND are the only instructions using arg2, and
// arg2's type always matches arg1's. because of this, arg2 isn't considered
// when generating the lookup table
#define CALLBACK_IDX(op, r, a0, a1)                                        \
  ((op)*VALUE_NUM * VALUE_NUM * VALUE_NUM) + ((r)*VALUE_NUM * VALUE_NUM) + \
      ((a0)*VALUE_NUM) + (a1)

// declare a templated callback for an IR operation. note, declaring a
// callback does not actually register it. callbacks must be registered
// for a particular signature with REGISTER_FOLD
#define FOLD(op, mask)                                                 \
  static struct _##op##_init {                                         \
    _##op##_init() { fold_masks[OP_##op] = mask; }                     \
  } op##_init;                                                         \
  template <typename R = void, typename A0 = void, typename A1 = void> \
  void Handle##op(IRBuilder &builder, Block *block, Instr *instr)

// registers a fold callback for the specified signature
#define REGISTER_FOLD(op, r, a0, a1)                                           \
  static struct _cpp_##op##_##r##_##a0##_##a1##_init {                         \
    _cpp_##op##_##r##_##a0##_##a1##_init() {                                   \
      fold_cbs[CALLBACK_IDX(OP_##op, VALUE_##r, VALUE_##a0, VALUE_##a1)] =     \
          &Handle##op<ValueType<VALUE_##r>::type, ValueType<VALUE_##a0>::type, \
                      ValueType<VALUE_##a1>::type>;                            \
    }                                                                          \
  } cpp_##op##_##r##_##a0##_##a1##_init

// common helpers for fold functions
#define ARG0() instr->arg0()->value<A0>()
#define ARG1() instr->arg1()->value<A1>()
#define ARG2() instr->arg2()->value<A1>()
#define RESULT(expr)                                                  \
  instr->result()->ReplaceRefsWith(builder.AllocConstant((R)(expr))); \
  block->RemoveInstr(instr)

static FoldFn GetFoldFn(Instr *instr) {
  auto it = fold_cbs.find(CALLBACK_IDX(
      instr->op(), instr->result() ? (int)instr->result()->type() : VALUE_V,
      instr->arg0() ? (int)instr->arg0()->type() : VALUE_V,
      instr->arg1() ? (int)instr->arg1()->type() : VALUE_V));
  if (it == fold_cbs.end()) {
    return nullptr;
  }
  return it->second;
}

static int GetFoldMask(Instr *instr) { return fold_masks[instr->op()]; }

static int GetConstantSig(Instr *instr) {
  int cnst_sig = 0;

  if (instr->arg0() && instr->arg0()->constant()) {
    cnst_sig |= ARG0_CNST;
  }

  if (instr->arg1() && instr->arg1()->constant()) {
    cnst_sig |= ARG1_CNST;
  }

  if (instr->arg2() && instr->arg2()->constant()) {
    cnst_sig |= ARG2_CNST;
  }

  return cnst_sig;
}

void ConstantPropagationPass::Run(IRBuilder &builder) {
  PROFILER_RUNTIME("ConstantPropagationPass::Run");

  for (auto block : builder.blocks()) {
    auto it = block->instrs().begin();
    auto end = block->instrs().end();

    while (it != end) {
      Instr *instr = *(it++);

      int fold_mask = GetFoldMask(instr);
      int cnst_sig = GetConstantSig(instr);
      if (!fold_mask || (cnst_sig & fold_mask) != fold_mask) {
        continue;
      }

      FoldFn fold = GetFoldFn(instr);
      if (!fold) {
        continue;
      }

      fold(builder, block, instr);
    }
  }
}

FOLD(SELECT, ARG0_CNST) {
  instr->result()->ReplaceRefsWith(ARG0() ? instr->arg1() : instr->arg2());
  block->RemoveInstr(instr);
}
REGISTER_FOLD(SELECT, I8, I8, I8);
REGISTER_FOLD(SELECT, I16, I16, I16);
REGISTER_FOLD(SELECT, I32, I32, I32);
REGISTER_FOLD(SELECT, I64, I64, I64);

FOLD(EQ, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() == ARG1()); }
REGISTER_FOLD(EQ, I8, I8, I8);
REGISTER_FOLD(EQ, I8, I16, I16);
REGISTER_FOLD(EQ, I8, I32, I32);
REGISTER_FOLD(EQ, I8, I64, I64);
REGISTER_FOLD(EQ, I8, F32, F32);
REGISTER_FOLD(EQ, I8, F64, F64);

FOLD(NE, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() != ARG1()); }
REGISTER_FOLD(NE, I8, I8, I8);
REGISTER_FOLD(NE, I8, I16, I16);
REGISTER_FOLD(NE, I8, I32, I32);
REGISTER_FOLD(NE, I8, I64, I64);
REGISTER_FOLD(NE, I8, F32, F32);
REGISTER_FOLD(NE, I8, F64, F64);

FOLD(SGE, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() >= ARG1()); }
REGISTER_FOLD(SGE, I8, I8, I8);
REGISTER_FOLD(SGE, I8, I16, I16);
REGISTER_FOLD(SGE, I8, I32, I32);
REGISTER_FOLD(SGE, I8, I64, I64);
REGISTER_FOLD(SGE, I8, F32, F32);
REGISTER_FOLD(SGE, I8, F64, F64);

FOLD(SGT, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() > ARG1()); }
REGISTER_FOLD(SGT, I8, I8, I8);
REGISTER_FOLD(SGT, I8, I16, I16);
REGISTER_FOLD(SGT, I8, I32, I32);
REGISTER_FOLD(SGT, I8, I64, I64);
REGISTER_FOLD(SGT, I8, F32, F32);
REGISTER_FOLD(SGT, I8, F64, F64);

// IR_OP(UGE)
// IR_OP(UGT)

FOLD(SLE, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() <= ARG1()); }
REGISTER_FOLD(SLE, I8, I8, I8);
REGISTER_FOLD(SLE, I8, I16, I16);
REGISTER_FOLD(SLE, I8, I32, I32);
REGISTER_FOLD(SLE, I8, I64, I64);
REGISTER_FOLD(SLE, I8, F32, F32);
REGISTER_FOLD(SLE, I8, F64, F64);

FOLD(SLT, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() < ARG1()); }
REGISTER_FOLD(SLT, I8, I8, I8);
REGISTER_FOLD(SLT, I8, I16, I16);
REGISTER_FOLD(SLT, I8, I32, I32);
REGISTER_FOLD(SLT, I8, I64, I64);
REGISTER_FOLD(SLT, I8, F32, F32);
REGISTER_FOLD(SLT, I8, F64, F64);

// IR_OP(ULE)
// IR_OP(ULT)

FOLD(ADD, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() + ARG1()); }
REGISTER_FOLD(ADD, I8, I8, I8);
REGISTER_FOLD(ADD, I16, I16, I16);
REGISTER_FOLD(ADD, I32, I32, I32);
REGISTER_FOLD(ADD, I64, I64, I64);
REGISTER_FOLD(ADD, F32, F32, F32);
REGISTER_FOLD(ADD, F64, F64, F64);

FOLD(SUB, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() - ARG1()); }
REGISTER_FOLD(SUB, I8, I8, I8);
REGISTER_FOLD(SUB, I16, I16, I16);
REGISTER_FOLD(SUB, I32, I32, I32);
REGISTER_FOLD(SUB, I64, I64, I64);
REGISTER_FOLD(SUB, F32, F32, F32);
REGISTER_FOLD(SUB, F64, F64, F64);

FOLD(SMUL, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() * ARG1()); }
REGISTER_FOLD(SMUL, I8, I8, I8);
REGISTER_FOLD(SMUL, I16, I16, I16);
REGISTER_FOLD(SMUL, I32, I32, I32);
REGISTER_FOLD(SMUL, I64, I64, I64);
REGISTER_FOLD(SMUL, F32, F32, F32);
REGISTER_FOLD(SMUL, F64, F64, F64);

FOLD(UMUL, ARG0_CNST | ARG1_CNST) {
  using U0 = typename std::make_unsigned<A0>::type;
  using U1 = typename std::make_unsigned<A1>::type;
  U0 lhs = static_cast<U0>(ARG0());
  U1 rhs = static_cast<U1>(ARG1());
  RESULT(static_cast<A0>(lhs * rhs));
}
REGISTER_FOLD(UMUL, I8, I8, I8);
REGISTER_FOLD(UMUL, I16, I16, I16);
REGISTER_FOLD(UMUL, I32, I32, I32);
REGISTER_FOLD(UMUL, I64, I64, I64);

// IR_OP(DIV)
// IR_OP(NEG)
// IR_OP(SQRT)
// IR_OP(ABS)
// IR_OP(SIN)
// IR_OP(COS)

FOLD(AND, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() & ARG1()); }
REGISTER_FOLD(AND, I8, I8, I8);
REGISTER_FOLD(AND, I16, I16, I16);
REGISTER_FOLD(AND, I32, I32, I32);
REGISTER_FOLD(AND, I64, I64, I64);

FOLD(OR, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() | ARG1()); }
REGISTER_FOLD(OR, I8, I8, I8);
REGISTER_FOLD(OR, I16, I16, I16);
REGISTER_FOLD(OR, I32, I32, I32);
REGISTER_FOLD(OR, I64, I64, I64);

FOLD(XOR, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() ^ ARG1()); }
REGISTER_FOLD(XOR, I8, I8, I8);
REGISTER_FOLD(XOR, I16, I16, I16);
REGISTER_FOLD(XOR, I32, I32, I32);
REGISTER_FOLD(XOR, I64, I64, I64);

FOLD(NOT, ARG0_CNST) { RESULT(~ARG0()); }
REGISTER_FOLD(NOT, I8, I8, V);
REGISTER_FOLD(NOT, I16, I16, V);
REGISTER_FOLD(NOT, I32, I32, V);
REGISTER_FOLD(NOT, I64, I64, V);

FOLD(SHL, ARG0_CNST | ARG1_CNST) { RESULT(ARG0() << ARG1()); }
REGISTER_FOLD(SHL, I8, I8, I32);
REGISTER_FOLD(SHL, I16, I16, I32);
REGISTER_FOLD(SHL, I32, I32, I32);
REGISTER_FOLD(SHL, I64, I64, I32);

// IR_OP(ASHR)

FOLD(LSHR, ARG0_CNST | ARG1_CNST) {
  using U0 = typename std::make_unsigned<A0>::type;
  RESULT((A0)((U0)ARG0() >> ARG1()));
}
REGISTER_FOLD(LSHR, I8, I8, I32);
REGISTER_FOLD(LSHR, I16, I16, I32);
REGISTER_FOLD(LSHR, I32, I32, I32);
REGISTER_FOLD(LSHR, I64, I64, I32);

// IR_OP(BRANCH)
// IR_OP(BRANCH_COND)
// IR_OP(CALL_EXTERNAL)
