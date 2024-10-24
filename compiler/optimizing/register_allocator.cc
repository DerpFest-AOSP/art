/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "register_allocator.h"

#include <iostream>
#include <sstream>

#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"
#include "base/bit_utils_iterator.h"
#include "base/bit_vector-inl.h"
#include "code_generator.h"
#include "register_allocator_linear_scan.h"
#include "ssa_liveness_analysis.h"

namespace art HIDDEN {

template <typename IsCalleeSave>
static uint32_t GetBlockedRegistersForCall(size_t num_registers, IsCalleeSave&& is_callee_save) {
  DCHECK_LE(num_registers, BitSizeOf<uint32_t>());
  uint32_t mask = 0u;
  for (size_t reg = 0; reg != num_registers; ++reg) {
    if (!is_callee_save(reg)) {
      mask |= 1u << reg;
    }
  }
  return mask;
}

static uint32_t GetBlockedCoreRegistersForCall(size_t num_registers, const CodeGenerator* codegen) {
  return GetBlockedRegistersForCall(
      num_registers, [&](size_t reg) { return codegen->IsCoreCalleeSaveRegister(reg); });
}

static uint32_t GetBlockedFpRegistersForCall(size_t num_registers, const CodeGenerator* codegen) {
  return GetBlockedRegistersForCall(
      num_registers, [&](size_t reg) { return codegen->IsFloatingPointCalleeSaveRegister(reg); });
}

RegisterAllocator::RegisterAllocator(ScopedArenaAllocator* allocator,
                                     CodeGenerator* codegen,
                                     const SsaLivenessAnalysis& liveness)
    : allocator_(allocator),
      codegen_(codegen),
      liveness_(liveness),
      num_core_registers_(codegen_->GetNumberOfCoreRegisters()),
      num_fp_registers_(codegen_->GetNumberOfFloatingPointRegisters()),
      core_registers_blocked_for_call_(
          GetBlockedCoreRegistersForCall(num_core_registers_, codegen)),
      fp_registers_blocked_for_call_(GetBlockedFpRegistersForCall(num_fp_registers_, codegen)) {}

std::unique_ptr<RegisterAllocator> RegisterAllocator::Create(ScopedArenaAllocator* allocator,
                                                             CodeGenerator* codegen,
                                                             const SsaLivenessAnalysis& analysis,
                                                             Strategy strategy) {
  switch (strategy) {
    case kRegisterAllocatorLinearScan:
      return std::unique_ptr<RegisterAllocator>(
          new (allocator) RegisterAllocatorLinearScan(allocator, codegen, analysis));
    case kRegisterAllocatorGraphColor:
      LOG(FATAL) << "Graph coloring register allocator has been removed.";
      UNREACHABLE();
    default:
      LOG(FATAL) << "Invalid register allocation strategy: " << strategy;
      UNREACHABLE();
  }
}

RegisterAllocator::~RegisterAllocator() {
  if (kIsDebugBuild) {
    // Poison live interval pointers with "Error: BAD 71ve1nt3rval."
    LiveInterval* bad_live_interval = reinterpret_cast<LiveInterval*>(0xebad7113u);
    for (HBasicBlock* block : codegen_->GetGraph()->GetLinearOrder()) {
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        it.Current()->SetLiveInterval(bad_live_interval);
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        it.Current()->SetLiveInterval(bad_live_interval);
      }
    }
  }
}

class AllRangesIterator : public ValueObject {
 public:
  explicit AllRangesIterator(LiveInterval* interval)
      : current_interval_(interval),
        current_range_(interval->GetFirstRange()) {}

  bool Done() const { return current_interval_ == nullptr; }
  LiveRange* CurrentRange() const { return current_range_; }
  LiveInterval* CurrentInterval() const { return current_interval_; }

  void Advance() {
    current_range_ = current_range_->GetNext();
    if (current_range_ == nullptr) {
      current_interval_ = current_interval_->GetNextSibling();
      if (current_interval_ != nullptr) {
        current_range_ = current_interval_->GetFirstRange();
      }
    }
  }

 private:
  LiveInterval* current_interval_;
  LiveRange* current_range_;

  DISALLOW_COPY_AND_ASSIGN(AllRangesIterator);
};

void RegisterAllocator::DumpRegister(std::ostream& stream,
                                     int reg,
                                     RegisterType register_type,
                                     const CodeGenerator* codegen) {
  switch (register_type) {
    case RegisterType::kCoreRegister:
      codegen->DumpCoreRegister(stream, reg);
      break;
    case RegisterType::kFpRegister:
      codegen->DumpFloatingPointRegister(stream, reg);
      break;
  }
}

uint32_t RegisterAllocator::GetRegisterMask(LiveInterval* interval,
                                            RegisterType register_type) const {
  if (interval->HasRegister()) {
    DCHECK_EQ(register_type == RegisterType::kFpRegister,
              DataType::IsFloatingPointType(interval->GetType()));
    DCHECK_LE(static_cast<size_t>(interval->GetRegister()), BitSizeOf<uint32_t>());
    return 1u << interval->GetRegister();
  } else if (interval->IsFixed()) {
    DCHECK_EQ(interval->GetType(), DataType::Type::kVoid);
    DCHECK(interval->GetFirstRange() != nullptr);
    size_t start = interval->GetFirstRange()->GetStart();
    bool blocked_for_call = liveness_.GetInstructionFromPosition(start / 2u) != nullptr;
    switch (register_type) {
      case RegisterType::kCoreRegister:
        return blocked_for_call ? core_registers_blocked_for_call_
                                : MaxInt<uint32_t>(num_core_registers_);
      case RegisterType::kFpRegister:
        return blocked_for_call ? fp_registers_blocked_for_call_
                                : MaxInt<uint32_t>(num_fp_registers_);
    }
  } else {
    return 0u;
  }
}

bool RegisterAllocator::ValidateIntervals(ArrayRef<LiveInterval* const> intervals,
                                          size_t number_of_spill_slots,
                                          size_t number_of_out_slots,
                                          const CodeGenerator& codegen,
                                          const SsaLivenessAnalysis* liveness,
                                          RegisterType register_type,
                                          bool log_fatal_on_failure) {
  size_t number_of_registers = (register_type == RegisterType::kCoreRegister)
      ? codegen.GetNumberOfCoreRegisters()
      : codegen.GetNumberOfFloatingPointRegisters();
  uint32_t registers_blocked_for_call = (register_type == RegisterType::kCoreRegister)
      ? GetBlockedCoreRegistersForCall(number_of_registers, &codegen)
      : GetBlockedFpRegistersForCall(number_of_registers, &codegen);

  // A copy of `GetRegisterMask()` using local `number_of_registers` and
  // `registers_blocked_for_call` instead of the cached per-type members
  // that we cannot use in this static member function.
  auto get_register_mask = [&](LiveInterval* interval) {
    if (interval->HasRegister()) {
      DCHECK_EQ(register_type == RegisterType::kFpRegister,
                DataType::IsFloatingPointType(interval->GetType()));
      DCHECK_LE(static_cast<size_t>(interval->GetRegister()), BitSizeOf<uint32_t>());
      return 1u << interval->GetRegister();
    } else if (interval->IsFixed()) {
      DCHECK_EQ(interval->GetType(), DataType::Type::kVoid);
      DCHECK(interval->GetFirstRange() != nullptr);
      size_t start = interval->GetFirstRange()->GetStart();
      CHECK(liveness != nullptr);
      bool blocked_for_call = liveness->GetInstructionFromPosition(start / 2u) != nullptr;
      return blocked_for_call ? registers_blocked_for_call
                              : MaxInt<uint32_t>(number_of_registers);
    } else {
      return 0u;
    }
  };

  ScopedArenaAllocator allocator(codegen.GetGraph()->GetArenaStack());
  ScopedArenaVector<ArenaBitVector*> liveness_of_values(
      allocator.Adapter(kArenaAllocRegisterAllocatorValidate));
  liveness_of_values.reserve(number_of_registers + number_of_spill_slots);

  size_t max_end = 0u;
  for (LiveInterval* start_interval : intervals) {
    for (AllRangesIterator it(start_interval); !it.Done(); it.Advance()) {
      max_end = std::max(max_end, it.CurrentRange()->GetEnd());
    }
  }

  // Allocate a bit vector per register. A live interval that has a register
  // allocated will populate the associated bit vector based on its live ranges.
  for (size_t i = 0; i < number_of_registers + number_of_spill_slots; ++i) {
    liveness_of_values.push_back(
        ArenaBitVector::Create(&allocator, max_end, false, kArenaAllocRegisterAllocatorValidate));
    liveness_of_values.back()->ClearAllBits();
  }

  for (LiveInterval* start_interval : intervals) {
    for (AllRangesIterator it(start_interval); !it.Done(); it.Advance()) {
      LiveInterval* current = it.CurrentInterval();
      HInstruction* defined_by = current->GetParent()->GetDefinedBy();
      if (current->GetParent()->HasSpillSlot()
           // Parameters and current method have their own stack slot.
           && !(defined_by != nullptr && (defined_by->IsParameterValue()
                                          || defined_by->IsCurrentMethod()))) {
        BitVector* liveness_of_spill_slot = liveness_of_values[number_of_registers
            + current->GetParent()->GetSpillSlot() / kVRegSize
            - number_of_out_slots];
        for (size_t j = it.CurrentRange()->GetStart(); j < it.CurrentRange()->GetEnd(); ++j) {
          if (liveness_of_spill_slot->IsBitSet(j)) {
            if (log_fatal_on_failure) {
              std::ostringstream message;
              message << "Spill slot conflict at " << j;
              LOG(FATAL) << message.str();
            } else {
              return false;
            }
          } else {
            liveness_of_spill_slot->SetBit(j);
          }
        }
      }

      for (uint32_t reg : LowToHighBits(get_register_mask(current))) {
        if (kIsDebugBuild && log_fatal_on_failure && !current->IsFixed()) {
          // Only check when an error is fatal. Only tests code ask for non-fatal failures
          // and test code may not properly fill the right information to the code generator.
          CHECK(codegen.HasAllocatedRegister(register_type == RegisterType::kCoreRegister, reg));
        }
        BitVector* liveness_of_register = liveness_of_values[reg];
        for (size_t j = it.CurrentRange()->GetStart(); j < it.CurrentRange()->GetEnd(); ++j) {
          if (liveness_of_register->IsBitSet(j)) {
            if (current->IsUsingInputRegister() && current->CanUseInputRegister()) {
              continue;
            }
            if (log_fatal_on_failure) {
              std::ostringstream message;
              message << "Register conflict at " << j << " ";
              if (defined_by != nullptr) {
                message << "(" << defined_by->DebugName() << ")";
              }
              message << "for ";
              DumpRegister(message, reg, register_type, &codegen);
              for (LiveInterval* interval : intervals) {
                if ((get_register_mask(interval) & (1u << reg)) != 0u && interval->CoversSlow(j)) {
                  message << std::endl;
                  if (interval->GetDefinedBy() != nullptr) {
                    message << interval->GetDefinedBy()->GetKind() << " ";
                  } else {
                    message << "physical ";
                  }
                  interval->Dump(message);
                }
              }
              LOG(FATAL) << message.str();
            } else {
              return false;
            }
          } else {
            liveness_of_register->SetBit(j);
          }
        }
      }
    }
  }
  return true;
}

LiveInterval* RegisterAllocator::Split(LiveInterval* interval, size_t position) {
  DCHECK_GE(position, interval->GetStart());
  DCHECK(!interval->IsDeadAt(position));
  if (position == interval->GetStart()) {
    // Spill slot will be allocated when handling `interval` again.
    interval->ClearRegister();
    if (interval->HasHighInterval()) {
      interval->GetHighInterval()->ClearRegister();
    } else if (interval->HasLowInterval()) {
      interval->GetLowInterval()->ClearRegister();
    }
    return interval;
  } else {
    LiveInterval* new_interval = interval->SplitAt(position);
    if (interval->HasHighInterval()) {
      LiveInterval* high = interval->GetHighInterval()->SplitAt(position);
      new_interval->SetHighInterval(high);
      high->SetLowInterval(new_interval);
    } else if (interval->HasLowInterval()) {
      LiveInterval* low = interval->GetLowInterval()->SplitAt(position);
      new_interval->SetLowInterval(low);
      low->SetHighInterval(new_interval);
    }
    return new_interval;
  }
}

LiveInterval* RegisterAllocator::SplitBetween(LiveInterval* interval, size_t from, size_t to) {
  HBasicBlock* block_from = liveness_.GetBlockFromPosition(from / 2);
  HBasicBlock* block_to = liveness_.GetBlockFromPosition(to / 2);
  DCHECK(block_from != nullptr);
  DCHECK(block_to != nullptr);

  // Both locations are in the same block. We split at the given location.
  if (block_from == block_to) {
    return Split(interval, to);
  }

  /*
   * Non-linear control flow will force moves at every branch instruction to the new location.
   * To avoid having all branches doing the moves, we find the next non-linear position and
   * split the interval at this position. Take the following example (block number is the linear
   * order position):
   *
   *     B1
   *    /  \
   *   B2  B3
   *    \  /
   *     B4
   *
   * B2 needs to split an interval, whose next use is in B4. If we were to split at the
   * beginning of B4, B3 would need to do a move between B3 and B4 to ensure the interval
   * is now in the correct location. It makes performance worst if the interval is spilled
   * and both B2 and B3 need to reload it before entering B4.
   *
   * By splitting at B3, we give a chance to the register allocator to allocate the
   * interval to the same register as in B1, and therefore avoid doing any
   * moves in B3.
   */
  if (block_from->GetDominator() != nullptr) {
    for (HBasicBlock* dominated : block_from->GetDominator()->GetDominatedBlocks()) {
      size_t position = dominated->GetLifetimeStart();
      if ((position > from) && (block_to->GetLifetimeStart() > position)) {
        // Even if we found a better block, we continue iterating in case
        // a dominated block is closer.
        // Note that dominated blocks are not sorted in liveness order.
        block_to = dominated;
        DCHECK_NE(block_to, block_from);
      }
    }
  }

  // If `to` is in a loop, find the outermost loop header which does not contain `from`.
  for (HLoopInformationOutwardIterator it(*block_to); !it.Done(); it.Advance()) {
    HBasicBlock* header = it.Current()->GetHeader();
    if (block_from->GetLifetimeStart() >= header->GetLifetimeStart()) {
      break;
    }
    block_to = header;
  }

  // Split at the start of the found block, to piggy back on existing moves
  // due to resolution if non-linear control flow (see `ConnectSplitSiblings`).
  return Split(interval, block_to->GetLifetimeStart());
}

}  // namespace art
