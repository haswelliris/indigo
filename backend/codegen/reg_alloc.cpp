#include "reg_alloc.hpp"

#include <any>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "../../include/aixlog.hpp"
#include "../optimization/graph_color.hpp"
#include "../optimization/optimization.hpp"

namespace backend::codegen {
using namespace arm;

const std::set<Reg> GP_REGS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

/// An interval represented by this struct is a semi-open interval
/// [start, end) where start means this value is first written and end means
/// this value is last read.
struct Interval {
  Interval(unsigned int point) : start(point), end(point) {}

  Interval(unsigned int start, unsigned int end) : start(start), end(end) {
    if (end < start) end = start;
  }

  unsigned int start;
  unsigned int end;

  void add_point(unsigned int pt) {
    add_starting_point(pt);
    add_ending_point(pt);
  }
  void add_starting_point(unsigned int start_) {
    if (start_ < start) start = start_;
  }
  void add_ending_point(unsigned int end_) {
    if (end_ > end) end = end_;
  }
  Interval with_starting_point(unsigned int start_) {
    Interval that = *this;
    that.start = start_;
    return that;
  }
  Interval with_ending_point(unsigned int end_) {
    Interval that = *this;
    end = end_;
    return that;
  }
  unsigned int length() { return end - start; }
  bool overlaps(const Interval other) {
    return end > other.start && start < other.end;
  }
};

struct SpillOperation {
  unsigned int index;
  bool is_store;
  Reg reg;

  // Sort by index, store<read and register number
  bool operator<(const SpillOperation &other) const {
    if (index != other.index) return index < other.index;
    if (is_store != other.is_store) return is_store;
    return reg < other.reg;
  }
};

struct Alloc {
  Reg reg;
  Interval interval;
};

enum class ReplaceWriteKind { Phys, Graph, Spill, Transient };
struct ReplaceWriteAction {
  Reg from;
  Reg replace_with;
  ReplaceWriteKind kind;
};

using ColorMap = ::optimization::graph_color::Color_Map;

class RegAllocator {
 public:
  RegAllocator(arm::Function &f, ColorMap &color_map,
               std::map<mir::inst::VarId, Reg> &mir_to_arm)
      : f(f),
        live_intervals(),
        reg_map(),
        stack_size(f.stack_size),
        spilled_regs(),
        spill_positions(),
        color_map(color_map),
        mir_to_arm(mir_to_arm),
        bb_used_regs(),
        inst_sink() {}
  arm::Function &f;
  ColorMap &color_map;
  optimization::MirVariableToArmVRegType::mapped_type &mir_to_arm;

  std::set<Reg> used_regs = {};
  std::set<Reg> used_regs_temp = {};
  std::unordered_map<uint32_t, std::set<Reg>> bb_used_regs;
  std::map<int, uint32_t> point_bb_map;

  std::unordered_map<Reg, Interval> live_intervals;
  std::unordered_map<Reg, Reg> reg_map;
  std::unordered_multimap<Reg, Reg> reg_reverse_map;
  // key: physical register; value: allocation interval
  std::unordered_map<Reg, Interval> active;
  // key: virtual register; value: physical register
  std::list<std::pair<Reg, Reg>> active_reg_map;
  std::unordered_map<Reg, Interval> spilled_regs;
  std::unordered_map<Reg, int> spill_positions;
  std::unordered_set<Reg> spilled_cross_block_reg;

  std::unordered_map<Reg, int> reg_assign_count;
  std::unordered_map<Reg, Reg> reg_affine;
  std::unordered_map<Reg, Reg> reg_collapse;

  //   std::multimap<int, SpillOperation> spill_operatons;
  std::vector<std::unique_ptr<arm::Inst>> inst_sink;
  std::set<int> bl_points;
  std::unordered_set<Reg> wrote_to = {};

  std::set<std::pair<Reg, int>> write_back_regs;

  int stack_size;
  int stack_offset = 0;
  std::optional<std::pair<Reg, Reg>> delayed_store;

  bool bb_reset = true;
  bool is_leaf_func = true;

  ConditionCode cur_cond = ConditionCode::Always;

#pragma region Read Write Stuff
  void add_reg_read(Operand2 &reg, unsigned int point) {
    if (auto x = std::get_if<RegisterOperand>(&reg)) {
      add_reg_read(x->reg, point);
    }
  }

  void add_reg_write(Operand2 &reg, unsigned int point) {
    if (auto x = std::get_if<RegisterOperand>(&reg)) {
      add_reg_write(x->reg, point);
    }
  }

  void add_reg_read(MemoryOperand &reg, unsigned int point) {
    add_reg_read(reg.r1, point);
    if (auto x = std::get_if<RegisterOperand>(&reg.offset)) {
      add_reg_read(x->reg, point);
    }
  }

  void add_reg_read(Reg reg, unsigned int point) {
    if (auto r = live_intervals.find(reg); r != live_intervals.end()) {
      r->second.add_ending_point(point);
    } else {
      live_intervals.insert({reg, Interval(point)});
    }
    add_reg_use_in_bb_at_point(reg, point);
  }

  void add_reg_write(Reg reg, unsigned int point) {
    if (auto r = live_intervals.find(reg); r != live_intervals.end()) {
      r->second.add_starting_point(point);
    } else {
      live_intervals.insert({reg, Interval(point)});
    }
    auto r_use_insert = reg_assign_count.insert({reg, 1});
    if (!r_use_insert.second) {
      r_use_insert.first->second++;
    };
    add_reg_use_in_bb_at_point(reg, point);
  }

  void add_reg_use_in_bb_at_point(Reg reg, unsigned int point) {
    auto r_mapped = reg_map.find(reg);
    if (r_mapped != reg_map.end()) {
      auto bb_iter = point_bb_map.lower_bound(point);
      bb_iter--;
      auto &reg_set = bb_used_regs.insert({bb_iter->second, {}}).first->second;
      reg_set.insert(r_mapped->second);
      used_regs.insert(r_mapped->second);
    }
  }

#pragma endregion
  void display_active_regs() {
    auto &trace = LOG(TRACE);
    trace << "active: ";
    for (auto x : active) {
      trace << x.first << "->[" << x.second.start << "," << x.second.end << "]"
            << "; ";
    }
    trace << std::endl;
    trace << "map: ";
    for (auto x : active_reg_map) {
      display_reg_name(trace, x.first);
      trace << "->" << x.second << "; ";
    }
    trace << std::endl;
  }

  void calc_live_intervals();
  void alloc_regs();
  void construct_reg_map();
  void calc_reg_affinity();

  std::vector<std::pair<Reg, Interval>> sort_intervals();
  //   void generate_load_store_positions(std::vector<std::pair<Reg,
  //   Interval>>);
  void replace_read(Reg &r, int i, std::optional<Reg> pre_alloc_transient = {});
  ReplaceWriteAction pre_replace_write(
      Reg &r, int i, std::optional<Reg> pre_alloc_transient = {});
  void replace_write(ReplaceWriteAction a, int i);
  void replace_read(Operand2 &r, int i);
  void replace_read(MemoryOperand &r, int i);

  void invalidate_read(int pos) {
    LOG(DEBUG) << "Invalidating: ";
    auto it = active.begin();
    while (it != active.end()) {
      if (it->second.end <= pos) {
        // The register is no longer to be read from, thus is freed
        auto ait = active_reg_map.begin();
        while (ait != active_reg_map.end()) {
          auto r = *ait;
          if (r.second == it->first) {
            active_reg_map.erase(ait);
            break;
          } else {
            ait++;
          }
        }
        LOG(DEBUG) << it->first << " ";
        it = active.erase(it);
      } else {
        it++;
      }
    }
    LOG(DEBUG) << std::endl;
  }
  Reg alloc_transient_reg(Interval i, std::optional<Reg> orig);
  Reg make_space(Reg r, Interval i);
  Reg alloc_read(Reg r);
  Reg alloc_write(Reg r);
  Reg get_collapse_reg(Reg r) {
    auto rd = reg_collapse.find(r);
    if (rd == reg_collapse.end())
      return r;
    else
      return get_collapse_reg(rd->second);
  }
  void force_free(Reg r, bool also_erase_map = true, bool write_back = true);
  int get_or_alloc_spill_pos(Reg r) {
    int pos;
    if (auto p = spill_positions.find(r); p != spill_positions.end()) {
      pos = p->second;
    } else {
      pos = stack_size;
      stack_size += 4;
      spill_positions.insert({r, pos});
    }
    return pos;
  }
  void perform_load_stores();
};

void RegAllocator::alloc_regs() {
  construct_reg_map();
  calc_live_intervals();

  LOG(TRACE, "bb_reg_use") << "BB starting point" << std::endl;
  for (auto x : point_bb_map) {
    LOG(TRACE, "bb_reg_use") << x.first << " -> " << x.second << std::endl;
  }

  // LOG(TRACE, "bb_reg_use") << "BB Reg use:" << std::endl;
  // for (auto x : bb_used_regs) {
  //   LOG(TRACE, "bb_reg_use") << x.first << " -> ";
  //   for (auto r : x.second) {
  //     display_reg_name(LOG(TRACE, "color_map"), r);
  //     LOG(TRACE, "bb_reg_use") << " ";
  //   }
  //   LOG(TRACE, "bb_reg_use") << std::endl;
  // }
  calc_reg_affinity();

  perform_load_stores();
  f.inst = std::move(inst_sink);

  {
    // Add used reg stuff
    auto &first = f.inst.front();
    auto first_ = static_cast<PushPopInst *>(&*first);
    for (auto r : used_regs) first_->regs.insert(r);
    for (auto r : used_regs_temp) first_->regs.insert(r);
    auto &last = f.inst.back();
    auto last_ = static_cast<PushPopInst *>(&*last);
    for (auto r : used_regs) last_->regs.insert(r);
    for (auto r : used_regs_temp) last_->regs.insert(r);

    auto use_stack_param = f.ty.get()->params.size() > 4;
    auto offset_size = (first_->regs.size()) * 4;

    if (!use_stack_param && stack_size == 0) {
      first_->regs.erase(REG_FP);
      last_->regs.erase(REG_FP);
    }

    if (use_stack_param) {
      f.inst.insert(f.inst.begin() + 2,
                    std::make_unique<Arith3Inst>(OpCode::Add, REG_FP, REG_FP,
                                                 Operand2(offset_size)));
    }

    if (stack_size == 0) {
      // sp does not change.
      if (!use_stack_param) {
        // no need to use fp
        f.inst.erase(f.inst.begin() + 1);
      }
    } else if (stack_size < 1024) {
      f.inst.insert(f.inst.begin() + 2,
                    std::make_unique<Arith3Inst>(OpCode::Sub, REG_SP, REG_SP,
                                                 Operand2(stack_size)));
    } else {
      f.inst.insert(
          f.inst.begin() + 2,
          std::make_unique<Arith2Inst>(OpCode::Mov, 12, Operand2(stack_size)));
      f.inst.insert(f.inst.begin() + 3,
                    std::make_unique<Arith3Inst>(OpCode::Sub, REG_SP, REG_SP,
                                                 RegisterOperand(12)));
    }

    if (stack_size == 0) {
      // sp hasn't change throught function
      f.inst.erase(f.inst.end() - 2);
    }

    if (use_stack_param) {
      f.inst.insert(f.inst.end() - 2,
                    std::make_unique<Arith3Inst>(OpCode::Sub, REG_FP, REG_FP,
                                                 Operand2(offset_size)));
    }
    {
      auto &first = f.inst.front();
      auto first_ = static_cast<PushPopInst *>(&*first);
      auto &last = f.inst.back();
      auto last_ = static_cast<PushPopInst *>(&*last);
      if (first_->regs.empty()) f.inst.erase(f.inst.begin());
      if (last_->regs.empty()) f.inst.erase(f.inst.end() - 1);
    }
  }
}

void RegAllocator::calc_live_intervals() {
  int curr_bb = 0;
  for (int i = 0; i < f.inst.size(); i++) {
    auto inst_ = &*f.inst[i];
    if (auto x = dynamic_cast<PureInst *>(inst_)) {
      //   noop
    } else if (auto x = dynamic_cast<Arith4Inst *>(inst_)) {
      add_reg_read(x->r1, i);
      add_reg_read(x->r2, i);
      add_reg_read(x->r3, i);
      add_reg_write(x->rd, i);
    } else if (auto x = dynamic_cast<Arith3Inst *>(inst_)) {
      add_reg_read(x->r1, i);
      add_reg_read(x->r2, i);
      add_reg_write(x->rd, i);
    } else if (auto x = dynamic_cast<Arith2Inst *>(inst_)) {
      if (x->op == arm::OpCode::Mov || x->op == arm::OpCode::MovT ||
          x->op == arm::OpCode::Mvn) {
        add_reg_write(x->r1, i);
        if (auto r2 = std::get_if<RegisterOperand>(&x->r2);
            x->op == arm::OpCode::Mov && r2 && r2->shift_amount == 0) {
          if (!is_virtual_register(x->r1) &&
              !is_virtual_register(x->r2.get_reg()))
            reg_affine.insert({x->r1, x->r2.get_reg()});
        }
      } else {
        add_reg_read(x->r1, i);
      }
      add_reg_read(x->r2, i);
    } else if (auto x = dynamic_cast<BrInst *>(inst_)) {
      if (x->op == arm::OpCode::Bl) {
        bl_points.insert(i);
      }
    } else if (auto x = dynamic_cast<LoadStoreInst *>(inst_)) {
      if (x->op == arm::OpCode::LdR) {
        add_reg_write(x->rd, i);
      } else {
        // StR
        add_reg_read(x->rd, i);
      }
      if (auto mem = std::get_if<MemoryOperand>(&x->mem)) add_reg_read(*mem, i);
    } else if (auto x = dynamic_cast<MultLoadStoreInst *>(inst_)) {
      if (x->op == arm::OpCode::LdM) {
        for (auto rd : x->rd) add_reg_write(rd, i);
      } else {
        // StM
        for (auto rd : x->rd) add_reg_read(rd, i);
      }
      add_reg_read(x->rn, i);
    } else if (auto x = dynamic_cast<PushPopInst *>(inst_)) {
      if (x->op == arm::OpCode::Push) {
        for (auto rd : x->regs) add_reg_write(rd, i);
      } else {
        // pop
        for (auto rd : x->regs) add_reg_read(rd, i);
      }
    } else if (auto x = dynamic_cast<LabelInst *>(inst_)) {
      if (x->label.find(".bb_") == 0) {
        try {
          auto bb_id_idx = x->label.find_last_of("$");
          auto id_str = x->label.substr(bb_id_idx + 1);
          auto bb_id = std::stoi(id_str);
          point_bb_map.insert({i, bb_id});
          curr_bb = bb_id;
        } catch (std::exception &i) {
          LOG(WARNING) << typeid(i).name() << ": " << i.what() << std::endl;
        }
      }
    } else
      //   noop
      ;
  }
}

void RegAllocator::construct_reg_map() {
  for (auto item : mir_to_arm) {
    // for every mir variable:
    auto [var_id, vreg_id] = item;
    auto color = color_map.find(var_id);
    if (color != color_map.end()) {
      if (color->second != -1) {
        // Global register id starts with r4;
        auto reg = GLOB_REGS[color->second];
        reg_map.insert({vreg_id, reg});
        reg_reverse_map.insert({reg, vreg_id});
        used_regs.insert(reg);
        {
          auto &trace = LOG(TRACE);
          trace << var_id << " <- ";
          display_reg_name(trace, vreg_id);
          trace << " <- ";
          display_reg_name(trace, reg);
          trace << std::endl;
        }
      } else {
        spill_positions.insert({vreg_id, stack_size});
        {
          auto &trace = LOG(TRACE);
          trace << "$" << var_id << " <- ";
          display_reg_name(trace, vreg_id);
          trace << " <- sp + " << stack_size << std::endl;
        }
        stack_size += 4;
        spilled_cross_block_reg.insert(vreg_id);
      }
    } else {
      // local variable
      {
        auto &trace = LOG(TRACE);
        trace << "$" << var_id << " <- ";
        display_reg_name(trace, vreg_id);
        trace << " <- local ";
        trace << std::endl;
      }
    }
  }
}

void RegAllocator::replace_read(Operand2 &r, int i) {
  if (auto rop = std::get_if<RegisterOperand>(&r)) {
    replace_read(rop->reg, i);
  }
}

void RegAllocator::replace_read(MemoryOperand &r, int i) {
  replace_read(r.r1, i);
  if (auto rop = std::get_if<RegisterOperand>(&r.offset)) {
    replace_read(rop->reg, i);
  }
}

Reg RegAllocator::alloc_transient_reg(Interval i, std::optional<Reg> orig) {
  Reg r = -1;
  if (orig) {
    LOG(TRACE) << "orig ";
    display_reg_name(LOG(TRACE), orig.value());
    LOG(TRACE) << " ";
    auto it = active_reg_map.begin();
    while (it != active_reg_map.end()) {
      if (it->first == orig)
        break;
      else
        it++;
    }
    if (it != active_reg_map.end()) {
      LOG(TRACE) << "r-> " << it->second << std::endl;
      auto r = it->second;
      auto x = *it;
      active_reg_map.erase(it);
      active_reg_map.push_back(x);
      return r;
    }
  }
  auto alloc_using_temp = [&]() {
    if (r == -1) {
      for (auto reg : TEMP_REGS) {
        if (active.find(reg) == active.end()) {
          r = reg;
          break;
        }
      }
    }
  };
  auto alloc_using_glob = [&]() {
    if (r == -1) {
      for (auto reg : GLOB_REGS) {
        if (active.find(reg) == active.end() &&
            used_regs.find(reg) == used_regs.end()) {
          r = reg;
          used_regs_temp.insert(reg);
          break;
        }
      }
    }
  };
  {
    auto lower_bound = bl_points.lower_bound(i.start);
    auto upper_bound = bl_points.upper_bound(i.end);
    if (lower_bound != upper_bound) {
      // Cross-BL virtual register
      alloc_using_glob();
      alloc_using_temp();
    } else {
      // non-cross-bl virtual register
      alloc_using_temp();
      alloc_using_glob();
    }
  }
  if (r == -1) {
    // Choose a value in active to spill.
    // NOTE: the current algorithm chooses the earliest-allocated register
    if (active_reg_map.size() == 0) {
      std::stringstream ss;
      ss << "Failed to allocate: all active registers are "
            "temporary!"
         << std::endl
         << "Dump: " << std::endl;

      for (auto [k, v] : active) {
        display_reg_name(ss, k);
        ss << ": [" << v.start << ", " << v.end << "]" << std::endl;
      }

      throw std::runtime_error(ss.str());
    }
    auto [spill_virt, spill_phys] = active_reg_map.front();
    active_reg_map.pop_front();
    auto interval = active.at(spill_phys);
    interval.start = i.start;
    int spill_pos = get_or_alloc_spill_pos(spill_virt);
    // TODO: move this into a separate function
    inst_sink.push_back(std::make_unique<LoadStoreInst>(
        OpCode::StR, spill_phys,
        MemoryOperand(REG_SP, spill_pos + stack_offset), cur_cond));

    auto &trace = LOG(TRACE);
    trace << "Spilling: ";
    display_reg_name(trace, spill_phys);
    trace << " -> ";
    display_reg_name(trace, spill_virt);
    trace << " -> " << spill_pos << " ";

    r = spill_phys;
    spilled_regs.insert({spill_virt, interval});
    {
      auto it = active_reg_map.begin();
      while (it != active_reg_map.end()) {
        if (it->first == orig) {
          active_reg_map.erase(it);
          break;
        } else
          it++;
      }
    }
    active.erase(spill_phys);
  }
  this->active.insert({r, i});
  if (orig) {
    this->active_reg_map.push_back({orig.value(), r});
  }
  LOG(TRACE) << "-> " << r << std::endl;
  display_active_regs();
  return r;
}

/// Replace virtual register r with real register in-place
void RegAllocator::replace_read(Reg &r, int i,
                                std::optional<Reg> pre_alloc_transient) {
  auto disp_reg = [r, i]() {
    display_reg_name(LOG(TRACE), r);
    LOG(TRACE) << " at: " << i << " ";
  };
  r = get_collapse_reg(r);
  if (!is_virtual_register(r)) {
    disp_reg();
    LOG(TRACE) << "phys" << std::endl;
    return;
  } else if (auto reg_map_r = reg_map.find(r); reg_map_r != reg_map.end()) {
    // This register is allocated with graph-coloring
    disp_reg();
    LOG(TRACE) << "graph " << reg_map_r->second << std::endl;
    r = reg_map_r->second;
    return;
  } else if (auto spill_r = spilled_regs.find(r);
             spill_r != spilled_regs.end()) {
    // this register is allocated in stack
    bool del = false;

    Reg rd;
    auto spill_pos = get_or_alloc_spill_pos(r);
    auto interval = spill_r->second;
    interval.start = i;
    spilled_regs.erase(r);
    if (pre_alloc_transient) {
      rd = pre_alloc_transient.value();
    } else {
      rd = alloc_transient_reg(interval, r);
    }

    if (inst_sink.size() > 0) {
      auto &x = inst_sink.back();
      if (auto x_ = dynamic_cast<LoadStoreInst *>(&*x)) {
        if (auto x__ = std::get_if<MemoryOperand>(&x_->mem);
            x_->op == arm::OpCode::StR && x_->rd == rd &&
            (*x__) == MemoryOperand(REG_SP, spill_pos + stack_offset) &&
            x->cond == cur_cond) {
          del = true;
        }
      }
    }
    if (del) {
      inst_sink.pop_back();
      delayed_store = {{r, rd}};
    } else {
      inst_sink.push_back(std::make_unique<LoadStoreInst>(
          OpCode::LdR, rd, MemoryOperand(REG_SP, spill_pos + stack_offset),
          cur_cond));
    }
    disp_reg();
    LOG(TRACE) << "spill " << spill_pos << "with rd=" << rd << std::endl;
    r = rd;
    return;
  } else {
    // is temporary register
    auto live_interval = live_intervals.at(r);
    r = alloc_transient_reg(live_interval, r);
    disp_reg();
    LOG(TRACE) << "transient ";
    display_reg_name(LOG(TRACE), r);
    LOG(TRACE) << std::endl;
    return;
  }
}

ReplaceWriteAction RegAllocator::pre_replace_write(
    Reg &r, int i, std::optional<Reg> pre_alloc_transient) {
  r = get_collapse_reg(r);
  auto r_ = r;
  if (!is_virtual_register(r)) {
    // is physical register; mark as occupied
    force_free(r);
    return {r, r, ReplaceWriteKind::Phys};
  } else if (auto reg_map_r = reg_map.find(r); reg_map_r != reg_map.end()) {
    r = reg_map_r->second;
    return {r_, reg_map_r->second, ReplaceWriteKind::Graph};
  } else if (spilled_cross_block_reg.find(r) != spilled_cross_block_reg.end()) {
    // When encountering write actions to cross-block regs, write immediately
    Reg rd;
    if (pre_alloc_transient) {
      rd = pre_alloc_transient.value();
    } else {
      auto it = active_reg_map.begin();
      while (it != active_reg_map.end()) {
        if (it->first == r_)
          break;
        else
          it++;
      }
      if (it != active_reg_map.end()) {
        rd = it->second;
        auto x = *it;
        active_reg_map.erase(it);
        active_reg_map.push_back(x);
      } else {
        auto interval = live_intervals.at(r).with_starting_point(i);
        ;
        rd = alloc_transient_reg(interval, r);
      }
    }

    r = rd;
    display_reg_name(LOG(TRACE), r_);
    LOG(TRACE) << " at: " << i << " to be spilled" << std::endl;
    return {r_, rd, ReplaceWriteKind::Spill};
  } else if (auto spill_r = spilled_regs.find(r);
             spill_r != spilled_regs.end()) {
    Reg rd;
    auto pos = get_or_alloc_spill_pos(r);
    auto interval = spill_r->second;
    interval.start = i;
    spilled_regs.erase(r);
    if (pre_alloc_transient) {
      rd = pre_alloc_transient.value();
    } else {
      rd = alloc_transient_reg(interval, r);
    }

    r = rd;
    display_reg_name(LOG(TRACE), r_);
    LOG(TRACE) << " at: " << i << " ";
    LOG(TRACE) << "spill " << pos << std::endl;
    return {r_, rd, ReplaceWriteKind::Spill};
  } else {
    // Is temporary register
    // the register should already be written to or read from
    auto live_interval = live_intervals.at(r);
    auto rd = alloc_transient_reg(live_interval, r);
    r = rd;
    return {r_, rd, ReplaceWriteKind::Transient};
    // throw new std::logic_error("Writing to transient register");
  }
}

void RegAllocator::replace_write(ReplaceWriteAction r, int i) {
  auto disp_reg = [&]() {
    display_reg_name(LOG(TRACE), r.from);
    LOG(TRACE) << " at: " << i << " ";
  };
  if (r.kind == ReplaceWriteKind::Phys) {
    // is physical register; mark as occupied
    active.insert({r.replace_with, Interval(i, UINT32_MAX)});
    disp_reg();
    LOG(TRACE) << "phys " << r.replace_with << std::endl;
    return;
  } else if (r.kind == ReplaceWriteKind::Graph) {
    // This register is allocated with graph-coloring
    disp_reg();
    LOG(TRACE) << "graph" << std::endl;
  } else if (r.kind == ReplaceWriteKind::Spill) {
    // this register is allocated in stack
    Reg rd = r.replace_with;
    int pos = get_or_alloc_spill_pos(r.from);

    bool del = false;
    if (inst_sink.size() > 0) {
      auto &x = inst_sink.back();
      if (auto x_ = dynamic_cast<LoadStoreInst *>(&*x)) {
        if (auto x__ = std::get_if<MemoryOperand>(&x_->mem);
            x_->op == arm::OpCode::StR && x__->r1 == rd &&
            x_->cond == cur_cond &&
            (*x__) == MemoryOperand(REG_SP, pos + stack_offset)) {
          del = true;
        }
      }
    }
    if (!del) {
      inst_sink.push_back(std::make_unique<LoadStoreInst>(
          OpCode::StR, rd, MemoryOperand(REG_SP, pos + stack_offset),
          cur_cond));
    }
    wrote_to.erase(r.from);
    disp_reg();
    LOG(TRACE) << "spill " << pos << " " << del << std::endl;
  } else {
    disp_reg();
    LOG(TRACE) << "temp" << std::endl;
    // Is temporary register
    // the register should already be written to or read from

    // throw new std::logic_error("Writing to transient register");
  }
}

void RegAllocator::force_free(Reg r, bool also_erase_map, bool write_back) {
  auto &trace = LOG(TRACE);
  display_reg_name(trace, r);
  if (auto x = active.find(r); x != active.end()) {
    for (auto y : active_reg_map) {
      if (y.second == r) {
        // Spill to stack
        int stack_pos = get_or_alloc_spill_pos(y.first);
        if (write_back)
          inst_sink.push_back(std::make_unique<LoadStoreInst>(
              OpCode::StR, r, MemoryOperand(REG_SP, stack_pos + stack_offset),
              cur_cond));
        spilled_regs.insert({y.first, x->second});
        trace << " " << y.first << " " << y.second << " @"
              << (stack_pos + stack_offset) << std::endl;
        active.erase(x);
        if (also_erase_map) {
          auto it = active_reg_map.begin();
          while (it != active_reg_map.end()) {
            if (it->first == y.first) {
              active_reg_map.erase(it);
              break;
            } else
              it++;
          }
        }
        return;
      }
    }
    trace << " Unable to find in active" << std::endl;
  } else {
    trace << " (not using)" << std::endl;
  }
}

Reg RegAllocator::make_space(Reg r, Interval i) {
  throw prelude::NotImplementedException();
}

std::vector<std::pair<Reg, Interval>> RegAllocator::sort_intervals() {
  //   Initialize vector
  std::vector<std::pair<Reg, Interval>> intervals;
  intervals.reserve(live_intervals.size());
  for (auto interval_pair : live_intervals) {
    intervals.push_back(interval_pair);
  }
  //   sort by interval starting point
  std::sort(intervals.begin(), intervals.end(), [](auto &first, auto &second) {
    return first.second.start < second.second.start;
  });

  return std::move(intervals);
}

void RegAllocator::calc_reg_affinity() {
  for (auto [reg_dst_, reg_src_] : reg_affine) {
    auto reg_dst = reg_dst_;
    auto reg_src = reg_src_;

    if (auto it = reg_map.find(reg_src);
        it != reg_map.end() && reg_map.find(reg_dst) == reg_map.end() &&
        spilled_cross_block_reg.find(reg_dst) ==
            spilled_cross_block_reg.end() &&
        reg_assign_count.at(reg_dst) == 1) {
      auto li_dst = live_intervals.at(reg_dst);
      bool overlaps = false;
      auto [rev_it, rev_it_end] = reg_reverse_map.equal_range(it->second);
      for (; rev_it != rev_it_end; rev_it++) {
        auto vr = rev_it->second;
        if (vr == reg_src) continue;
        auto interval = live_intervals.at(vr);
        if (interval.overlaps(li_dst)) {
          overlaps = true;
          break;
        }
      }
      if (!overlaps) {
        reg_collapse.insert({reg_dst, reg_src});
      }
    } else if (auto it = reg_map.find(reg_dst);
               it != reg_map.end() && reg_map.find(reg_src) == reg_map.end() &&
               spilled_cross_block_reg.find(reg_src) ==
                   spilled_cross_block_reg.end()) {
      auto li_src = live_intervals.at(reg_src);
      bool overlaps = false;
      auto [rev_it, rev_it_end] = reg_reverse_map.equal_range(it->second);
      for (; rev_it != rev_it_end; rev_it++) {
        auto vr = rev_it->second;
        if (vr == reg_src) continue;
        auto interval = live_intervals.at(vr);
        if (interval.overlaps(li_src)) {
          overlaps = true;
          break;
        }
      }
      if (!overlaps) {
        reg_collapse.insert({reg_src_, reg_dst_});
      }
    } else if (reg_map.find(reg_src) == reg_map.end() &&
               spilled_cross_block_reg.find(reg_src) ==
                   spilled_cross_block_reg.end() &&
               reg_map.find(reg_dst) == reg_map.end() &&
               spilled_cross_block_reg.find(reg_dst) ==
                   spilled_cross_block_reg.end()) {
      //  Both are local variables
      auto reg_src_ = get_collapse_reg(reg_src);
      auto reg_dst_ = get_collapse_reg(reg_dst);
      auto &li_src = live_intervals.at(reg_src_);
      auto &li_dst = live_intervals.at(reg_dst_);
      if (!li_src.overlaps(li_dst)) {
        li_src.add_starting_point(li_dst.start);
        li_src.add_ending_point(li_dst.end);
        reg_collapse.insert({reg_dst_, reg_src_});
      }
    }
  }
}

void RegAllocator::perform_load_stores() {
  for (int i = 0; i < f.inst.size(); i++) {
    auto inst_ = &*f.inst[i];
    LOG(TRACE) << " " << std::endl << *inst_ << std::endl;
    cur_cond = inst_->cond;
    if (auto x = dynamic_cast<Arith3Inst *>(inst_)) {
      replace_read(x->r1, i);
      replace_read(x->r2, i);
      invalidate_read(i);
      wrote_to.insert(x->rd);
      auto prw = pre_replace_write(x->rd, i);
      inst_sink.push_back(std::move(f.inst[i]));
      replace_write(prw, i);
    } else if (auto x = dynamic_cast<Arith4Inst *>(inst_)) {
      replace_read(x->r1, i);
      replace_read(x->r2, i);
      replace_read(x->r3, i);
      invalidate_read(i);
      wrote_to.insert(x->rd);
      auto prw = pre_replace_write(x->rd, i);
      inst_sink.push_back(std::move(f.inst[i]));
      replace_write(prw, i);
    } else if (auto x = dynamic_cast<Arith2Inst *>(inst_)) {
      if (x->op == arm::OpCode::Mov || x->op == arm::OpCode::Mvn) {
        replace_read(x->r2, i);
        invalidate_read(i);
        wrote_to.insert(x->r1);
        auto prw = pre_replace_write(x->r1, i);
        inst_sink.push_back(std::move(f.inst[i]));
        replace_write(prw, i);
      } else if (x->op == arm::OpCode::MovT) {
        auto r = x->r1;
        replace_read(x->r1, i);
        invalidate_read(i);
        wrote_to.insert(x->r1);
        auto prw = pre_replace_write(r, i, x->r1);
        inst_sink.push_back(std::move(f.inst[i]));
        replace_write(prw, i);
      } else {
        replace_read(x->r1, i);
        replace_read(x->r2, i);
        invalidate_read(i);
        inst_sink.push_back(std::move(f.inst[i]));
      }
    } else if (auto x = dynamic_cast<LoadStoreInst *>(inst_)) {
      if (auto mem = std::get_if<MemoryOperand>(&x->mem)) {
        replace_read(*mem, i);
      }
      if (x->op == arm::OpCode::LdR) {
        invalidate_read(i);
        wrote_to.insert(x->rd);
        auto prw = pre_replace_write(x->rd, i);
        inst_sink.push_back(std::move(f.inst[i]));
        replace_write(prw, i);
      } else {
        // StR
        replace_read(x->rd, i);
        invalidate_read(i);
        inst_sink.push_back(std::move(f.inst[i]));
      }
    } else if (auto x = dynamic_cast<MultLoadStoreInst *>(inst_)) {
      throw prelude::NotImplementedException();
      if (x->op == arm::OpCode::LdM) {
        for (auto rd : x->rd) add_reg_write(rd, i);
      } else {
        // StM
        for (auto rd : x->rd) add_reg_read(rd, i);
      }
      invalidate_read(i);
      add_reg_read(x->rn, i);
    } else if (auto x = dynamic_cast<PushPopInst *>(inst_)) {
      // push pop only use gpr
      invalidate_read(i);
      inst_sink.push_back(std::move(f.inst[i]));
    } else if (auto x = dynamic_cast<LabelInst *>(inst_)) {
      invalidate_read(i);

      inst_sink.push_back(std::move(f.inst[i]));
      if (x->label.find(".ld_pc") == 0 && inst_sink.size() >= 2 &&
          dynamic_cast<LoadStoreInst *>(&**(inst_sink.end() - 2))) {
        // HACK: If it's load_pc label, delay store once more
        std::swap(*(inst_sink.end() - 2), *(inst_sink.end() - 1));
      }
      if (x->label.find(".bb" == 0)) {
        bb_reset = true;
      }
    } else if (auto x = dynamic_cast<BrInst *>(inst_)) {
      if (delayed_store) {
        // TODO: check if this is right
        auto [r, rd] = delayed_store.value();
        replace_write({r, rd, ReplaceWriteKind::Spill}, i);
        delayed_store = {};
      }
      invalidate_read(i);
      if (x->op == arm::OpCode::Bl) {
        is_leaf_func = false;
        auto &label = x->l;
        int param_cnt = x->param_cnt;
        int reg_cnt = std::min(param_cnt, 4);
        for (int i = 0; i < reg_cnt; i++) active.erase(Reg(i));
        for (int i = reg_cnt; i < 4; i++) force_free(Reg(i));
        // R12 should be freed whatever condition
        force_free(Reg(12));
        force_free(Reg(REG_LR));
        inst_sink.push_back(std::move(f.inst[i]));
        active.erase(Reg(0));
        active.erase(Reg(1));
        active.erase(Reg(2));
        active.erase(Reg(3));
        active.erase(Reg(12));
        active.erase(Reg(REG_LR));
      } else if (x->op == arm::OpCode::B) {
        if (bb_reset) {
          auto it = active_reg_map.begin();

          while (it != active_reg_map.end()) {
            if (spilled_cross_block_reg.find(it->first) !=
                spilled_cross_block_reg.end()) {
              force_free(it->second, false,
                         wrote_to.find(it->first) != wrote_to.end());
              active.erase(it->second);
              it = active_reg_map.erase(it);
            } else {
              it++;
            }
          }
          wrote_to.clear();
          bb_reset = false;
        }
        inst_sink.push_back(std::move(f.inst[i]));
      } else {
        inst_sink.push_back(std::move(f.inst[i]));
      }
    } else if (auto x = dynamic_cast<CtrlInst *>(inst_)) {
      if (x->key == "offset_stack") {
        int offset = std::any_cast<int>(x->val);
        stack_offset += offset;
      }
      invalidate_read(i);
      inst_sink.push_back(std::move(f.inst[i]));
    } else {
      invalidate_read(i);
      inst_sink.push_back(std::move(f.inst[i]));
    }
    if (delayed_store) {
      // TODO: check if this is right
      auto [r, rd] = delayed_store.value();
      replace_write({r, rd, ReplaceWriteKind::Spill}, i);
      delayed_store = {};
    }
  }
}

void RegAllocatePass::optimize_arm(
    arm::ArmCode &arm_code, std::map<std::string, std::any> &extra_data_repo) {
  for (auto &f : arm_code.functions) {
    optimize_func(*f, extra_data_repo);
  }
}

void RegAllocatePass::optimize_func(
    arm::Function &f, std::map<std::string, std::any> &extra_data_repo) {
  auto &var_mapping_data =
      std::any_cast<optimization::MirVariableToArmVRegType &>(
          extra_data_repo.at(optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME));

  auto &coloring_data = std::any_cast<
      std::unordered_map<std::string, std::shared_ptr<ColorMap>> &>(
      extra_data_repo.at("graph_color"));

  auto f_coloring_data = coloring_data.find(f.name);
  auto var_mapping = var_mapping_data.find(f.name);

  RegAllocator fal(f, *f_coloring_data->second, var_mapping->second);
  fal.alloc_regs();
}

}  // namespace backend::codegen
