#pragma once

#include <string>
#include <unordered_map>

#include "../backend.hpp"

namespace optimization {

const std::string BASIC_BLOCK_ORDERING_DATA_NAME = "basic_block_ordering";

using BasicBlockOrderingType =
    std::unordered_map<std::string, std::vector<uint32_t>>;

const std::string MIR_VARIABLE_TO_ARM_VREG_DATA_NAME = "mir_variable_to_vreg";

using MirVariableToArmVRegType =
    std::unordered_map<std::string, std::map<mir::inst::VarId, arm::Reg>>;

}  // namespace optimization