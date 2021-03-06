#include "arm.hpp"

#include <any>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

#include "../prelude/prelude.hpp"

namespace arm {

RegisterKind register_type(Reg r) {
  if (r < 16)
    return RegisterKind::GeneralPurpose;
  else if (r < 48)
    return RegisterKind::DoubleVector;
  else if (r < 64)
    return RegisterKind::QuadVector;
  else if (r < 1 << 31)
    return RegisterKind::VirtualGeneralPurpose;
  else if (r < 3 << 30)
    return RegisterKind::VirtualDoubleVector;
  else
    return RegisterKind::VirtualQuadVector;
}

uint32_t register_num(Reg r) {
  if (r < 16)
    return r;
  else if (r < 48)
    return r - 16;
  else if (r < 64)
    return r - 48;
  else if (r < 1 << 31)
    return r - 64;
  else if (r < 3 << 30)
    return r - (1 << 31);
  else
    return r - (3 << 30);
}

void display_reg_name(std::ostream &o, Reg r) {
  if (r == REG_SP)
    o << "sp";
  else if (r == REG_LR)
    o << "lr";
  else if (r == REG_PC)
    o << "pc";
  else
    switch (register_type(r)) {
      case RegisterKind::GeneralPurpose:
        o << "r" << register_num(r);
        break;
      case RegisterKind::DoubleVector:
        o << "d" << register_num(r);
        break;
      case RegisterKind::QuadVector:
        o << "q" << register_num(r);
        break;
      case RegisterKind::VirtualGeneralPurpose:
        o << "v" << register_num(r);
        break;
      case RegisterKind::VirtualDoubleVector:
        o << "vd" << register_num(r);
        break;
      case RegisterKind::VirtualQuadVector:
        o << "vq" << register_num(r);
        break;
    }
}

void display_shift(std::ostream &o, RegisterShiftKind shift) {
  switch (shift) {
    case RegisterShiftKind::Lsl:
      o << "LSL";
      break;
    case RegisterShiftKind::Lsr:
      o << "LSR";
      break;
    case RegisterShiftKind::Asr:
      o << "ASR";
      break;
    case RegisterShiftKind::Ror:
      o << "ROR";
      break;
    case RegisterShiftKind::Rrx:
      o << "RRX";
      break;
  }
}

Reg make_register(RegisterKind k, uint32_t num) {
  switch (k) {
    case RegisterKind::GeneralPurpose:
      return num + REG_GP_START;
    case RegisterKind::DoubleVector:
      return num + REG_DOUBLE_START;
    case RegisterKind::QuadVector:
      return num + REG_QUAD_START;
    case RegisterKind::VirtualGeneralPurpose:
      return num + REG_V_GP_START;
    case RegisterKind::VirtualDoubleVector:
      return num + REG_V_DOUBLE_START;
    case RegisterKind::VirtualQuadVector:
      return num + REG_V_QUAD_START;
    default:
      return num;
  }
}

void RegisterOperand::display(std::ostream &o) const {
  display_reg_name(o, reg);
  if (shift != RegisterShiftKind::Lsl || shift_amount != 0) {
    o << ", ";
    display_shift(o, shift);
    if (shift != RegisterShiftKind::Rrx) {
      o << " #" << (unsigned int)shift_amount;
    }
  }
}

bool is_valid_immediate(uint32_t val) {
  if (val <= 0xff)
    return true;
  else if (val <= 0x00ffffff) {
    int highest_bit = log2(val & -val) + 1;
    return (val & ~(0xff << highest_bit)) == 0 && highest_bit % 2 == 0;
  } else {
    val = prelude::rotl32(val, 8);
    int highest_bit = log2(val & -val) + 1;
    return (val & ~(0xff << highest_bit)) == 0 && highest_bit % 2 == 0;
  }
}

void Operand2::display(std::ostream &o) const {
  if (auto x = std::get_if<RegisterOperand>(this)) {
    x->display(o);
  } else if (auto x = std::get_if<int32_t>(this)) {
    o << "#" << *x;
  }
}

void display_op(OpCode op, std::ostream &o) {
  switch (op) {
    case OpCode::Nop:
      o << "nop";
      break;
    case OpCode::B:
      o << "b";
      break;
    case OpCode::Bl:
      o << "bl";
      break;
    case OpCode::Bx:
      o << "bx";
      break;
    case OpCode::Cbz:
      o << "cbz";
      break;
    case OpCode::Cbnz:
      o << "cbnz";
      break;
    case OpCode::Mov:
      o << "mov";
      break;
    case OpCode::MovT:
      o << "movt";
      break;
    case OpCode::Mvn:
      o << "mvn";
      break;
    case OpCode::Add:
      o << "add";
      break;
    case OpCode::Sub:
      o << "sub";
      break;
    case OpCode::Rsb:
      o << "rsb";
      break;
    case OpCode::Mul:
      o << "mul";
      break;
    case OpCode::SMMul:
      o << "smmul";
      break;
    case OpCode::Mla:
      o << "mla";
      break;
    case OpCode::SMMla:
      o << "smmla";
      break;
    case OpCode::SDiv:
      o << "sdiv";
      break;
    case OpCode::Lsl:
      o << "lsl";
      break;
    case OpCode::Lsr:
      o << "lsr";
      break;
    case OpCode::Asr:
      o << "asr";
      break;
    case OpCode::And:
      o << "and";
      break;
    case OpCode::Orr:
      o << "orr";
      break;
    case OpCode::Eor:
      o << "eor";
      break;
    case OpCode::Bic:
      o << "bic";
      break;
    case OpCode::Cmp:
      o << "cmp";
      break;
    case OpCode::Cmn:
      o << "cmn";
      break;
    case OpCode::LdR:
      o << "ldr";
      break;
    case OpCode::LdM:
      o << "ldm";
      break;
    case OpCode::StR:
      o << "str";
      break;
    case OpCode::StM:
      o << "stm";
      break;
    case OpCode::Push:
      o << "push";
      break;
    case OpCode::Pop:
      o << "pop";
      break;
    case OpCode::_Label:
      // Labels are pseudo-instructions
      break;
    case OpCode::_Mod:
      o << "_MOD";
      break;
    default:
      o << "?" << (int)op;
      break;
  }
}

void display_cond(ConditionCode cond, std::ostream &o) {
  switch (cond) {
    case ConditionCode::Equal:
      o << "eq";
      break;
    case ConditionCode::NotEqual:
      o << "ne";
      break;
    case ConditionCode::CarrySet:
      o << "cs";
      break;
    case ConditionCode::CarryClear:
      o << "cc";
      break;
    case ConditionCode::UnsignedGe:
      o << "hs";
      break;
    case ConditionCode::UnsignedLe:
      o << "ls";
      break;
    case ConditionCode::UnsignedGt:
      o << "hi";
      break;
    case ConditionCode::UnsignedLt:
      o << "lo";
      break;
    case ConditionCode::MinusOrNegative:
      o << "mn";
      break;
    case ConditionCode::PositiveOrZero:
      o << "pl";
      break;
    case ConditionCode::Overflow:
      o << "vs";
      break;
    case ConditionCode::NoOverflow:
      o << "vc";
      break;
    case ConditionCode::Ge:
      o << "ge";
      break;
    case ConditionCode::Lt:
      o << "lt";
      break;
    case ConditionCode::Gt:
      o << "gt";
      break;
    case ConditionCode::Le:
      o << "le";
      break;
    case ConditionCode::Always:
      // AL is default
      break;
  }
}

ConditionCode invert_cond(ConditionCode cond) {
  switch (cond) {
    case ConditionCode::Equal:
      return ConditionCode::NotEqual;
    case ConditionCode::NotEqual:
      return ConditionCode::Equal;

    case ConditionCode::CarrySet:
      return ConditionCode::CarryClear;
    case ConditionCode::CarryClear:
      return ConditionCode::CarrySet;

    case ConditionCode::UnsignedGe:
      return ConditionCode::UnsignedLt;
    case ConditionCode::UnsignedLt:
      return ConditionCode::UnsignedGe;
    case ConditionCode::UnsignedGt:
      return ConditionCode::UnsignedLe;
    case ConditionCode::UnsignedLe:
      return ConditionCode::UnsignedGt;

    case ConditionCode::MinusOrNegative:
      return ConditionCode::PositiveOrZero;
    case ConditionCode::PositiveOrZero:
      return ConditionCode::MinusOrNegative;

    case ConditionCode::Overflow:
      return ConditionCode::NoOverflow;
    case ConditionCode::NoOverflow:
      return ConditionCode::Overflow;

    case ConditionCode::Ge:
      return ConditionCode::Lt;
    case ConditionCode::Lt:
      return ConditionCode::Ge;
    case ConditionCode::Gt:
      return ConditionCode::Le;
    case ConditionCode::Le:
      return ConditionCode::Gt;

    case ConditionCode::Always:
    default:
      return ConditionCode::Always;
  }
}

ConditionCode reverse_cond(ConditionCode cond) {
  switch (cond) {
    case ConditionCode::Equal:
    case ConditionCode::NotEqual:
    case ConditionCode::CarrySet:
    case ConditionCode::CarryClear:
    case ConditionCode::MinusOrNegative:
    case ConditionCode::PositiveOrZero:
    case ConditionCode::Overflow:
    case ConditionCode::NoOverflow:
    case ConditionCode::Always:
      return cond;

    case ConditionCode::UnsignedGe:
      return ConditionCode::UnsignedLe;
    case ConditionCode::UnsignedLt:
      return ConditionCode::UnsignedGt;
    case ConditionCode::UnsignedGt:
      return ConditionCode::UnsignedLt;
    case ConditionCode::UnsignedLe:
      return ConditionCode::UnsignedGe;

    case ConditionCode::Ge:
      return ConditionCode::Le;
    case ConditionCode::Lt:
      return ConditionCode::Gt;
    case ConditionCode::Gt:
      return ConditionCode::Lt;
    case ConditionCode::Le:
      return ConditionCode::Ge;
    default:
      return cond;
  }
}

std::string format_bb_name(std::string_view func_name, uint32_t bb_id) {
  std::stringstream name;
  name << func_name << "_$bb" << bb_id;
  return name.str();
}

void ConstValue::display(std::ostream &o) const {
  if (auto x = std::get_if<uint32_t>(this)) {
    o << "\t.word " << *x;
  } else if (auto x = std::get_if<std::vector<uint32_t>>(this)) {
    uint32_t last = x->front();
    int repeat_count = 1;
    bool newline = true;
    for (int i = 1; i < x->size(); i++) {
      uint32_t new_item = (*x)[i];
      if (newline) {
        if (new_item == last) {
          repeat_count++;
        } else {
          o << std::endl << "\t.word " << last;
        }
        newline = false;
      } else {
        if (new_item == last) {
          repeat_count++;
        } else {
          if (repeat_count > 1) {
            o << std::endl << "\t.fill " << repeat_count << ", 4, " << last;
            newline = true;
            repeat_count = 1;
          } else {
            o << ", " << last;
          }
        }
      }
      last = new_item;
    }
    if (repeat_count > 1) {
      o << std::endl;
      o << "\t.fill " << repeat_count << ", 4, " << last << std::endl;
      newline = true;
      repeat_count = 1;
    } else {
      o << ", " << last << std::endl;
    }
    if (len && x->size() < len.value()) {
      o << "\t.fill " << (len.value() - x->size()) << ", 4, " << last
        << std::endl;
    }
  } else if (auto x = std::get_if<std::string>(this)) {
    if (ty == ConstType::AsciZ)
      o << "\t.asciz \"" << *x << "\"";
    else if (ty == ConstType::Word)
      o << "\t.word " << *x;
  }
}

size_t ConstValue::size() {
  if (auto x = std::get_if<uint32_t>(this)) {
    return 4;
  } else if (auto x = std::get_if<std::vector<uint32_t>>(this)) {
    if (len) {
      return *len;
    } else {
      return x->size();
    }
  } else if (auto x = std::get_if<std::string>(this)) {
    return x->size();
  } else {
    throw std::logic_error("No such variant");
  }
}

void MemoryOperand::display(std::ostream &o) const {
  auto display_offset = [&](std::ostream &o) {
    if (auto x = std::get_if<RegisterOperand>(&offset)) {
      if (neg_rm) o << "-";
      o << *x;
    } else if (auto x = std::get_if<int16_t>(&offset)) {
      o << "#" << *x;
    }
  };
  o << "[";
  display_reg_name(o, r1);
  switch (kind) {
    case MemoryAccessKind::None:
      o << ", ";
      display_offset(o);
      o << "]";
      break;
    case MemoryAccessKind::PostIndex:
      o << ", ";
      display_offset(o);
      o << "]!";
      break;
    case MemoryAccessKind::PreIndex:
      o << "], ";
      display_offset(o);
      break;
  }
}

void PureInst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
}

void Arith2Inst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " ";
  display_reg_name(o, r1);
  if (op != OpCode::Bx) {
    o << ", " << r2;
  }
}

void Arith3Inst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " ";
  display_reg_name(o, rd);
  o << ", ";
  display_reg_name(o, r1);
  o << ", " << r2;
}

void Arith4Inst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " ";
  display_reg_name(o, rd);
  o << ", ";
  display_reg_name(o, r1);
  o << ", ";
  display_reg_name(o, r2);
  o << ", ";
  display_reg_name(o, r3);
}

void BrInst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " " << l;
}

void LoadStoreInst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " ";
  display_reg_name(o, rd);
  o << ", ";
  if (auto m = std::get_if<std::string>(&mem)) {
    o << *m;
  } else if (auto m = std::get_if<MemoryOperand>(&mem)) {
    o << *m;
  }
}

void MultLoadStoreInst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " ";
  display_reg_name(o, rn);
  o << ", {";
  for (auto i = rd.begin(); i != rd.end(); i++) {
    if (i != rd.begin()) o << ", ";
    display_reg_name(o, *i);
  }
  o << "}";
}

void PushPopInst::display(std::ostream &o) const {
  display_op(op, o);
  display_cond(cond, o);
  o << " {";
  for (auto i = regs.begin(); i != regs.end(); i++) {
    if (i != regs.begin()) o << ", ";
    display_reg_name(o, *i);
  }
  o << "}";
}

void LabelInst::display(std::ostream &o) const { o << label << ":"; }

#define ctrl_inst_display_type(v, ty)              \
  if (v.type() == typeid(ty)) {                    \
    o << "(value=" << std::any_cast<ty>(v) << ")"; \
  }

#define ctrl_inst_display_type_a(v, ty) \
  if (v.type() == typeid(ty)) {         \
    o << std::any_cast<ty>(v);          \
  }

void CtrlInst::display(std::ostream &o) const {
  if (is_asm_option) {
    o << "." << key << " ";
    ctrl_inst_display_type_a(val, int);
    ctrl_inst_display_type_a(val, double);
    ctrl_inst_display_type_a(val, float);
    ctrl_inst_display_type_a(val, long);
    ctrl_inst_display_type_a(val, std::string);
  } else {
    o << "@ " << key << "<" << val.type().name() << ">";
    ctrl_inst_display_type(val, int);
    ctrl_inst_display_type(val, double);
    ctrl_inst_display_type(val, float);
    ctrl_inst_display_type(val, long);
    ctrl_inst_display_type(val, std::string);
  }
}

void Function::display(std::ostream &o) const {
  for (auto &v : this->local_const) {
    o << v.first << ":" << std::endl;
    o << "\t" << v.second << std::endl;
  }

  o << "\t.globl " << name << std::endl;

  o << "\t@ " << name << ": " << *ty << std::endl;

  o << name << ":" << std::endl;
  // o << "\t.align 7" << std::endl;
  o << "\t.fnstart" << std::endl;
  for (auto &i : inst) {
    if (i == nullptr) {
      o << "!!!nullptr!!!" << std::endl;
      continue;
    } else if (dynamic_cast<LabelInst *>(&*i) == nullptr) {
      o << "\t";
    }
    o << *i << std::endl;
  }
  o << "\t.fnend" << std::endl;
}

void ArmCode::display(std::ostream &o) const {
  o << ".text" << std::endl;
  for (auto &f : functions) {
    o << *f << std::endl;
  }

  o << ".data" << std::endl;
  for (auto &v : this->consts) {
    o << v.first << ":" << std::endl;
    o << v.second << std::endl;
  }
  o << std::endl;
}

}  // namespace arm
