#include "game/native/coordinate_pool_internal/FindDec.h"
#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

namespace lengjing::game::native::coordinate_pool_internal {

namespace {
	// Flattened variants may place the stride constant well before the two
	// slot-address MADDs.  Keep the search inside the current basic block, but
	// allow enough instructions for the expanded index calculation.
	constexpr uint32_t kRingStrideSearchBack = 128;

	arm64_reg normalize_gp_reg(arm64_reg reg) {
		if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30) {
			return static_cast<arm64_reg>(reg - ARM64_REG_W0 + ARM64_REG_X0);
		}
		return reg;
	}

	bool is_control_flow(const cs_insn* insn) {
		if (!insn) return true;
		switch (insn->id) {
		case ARM64_INS_B:
		case ARM64_INS_BR:
		case ARM64_INS_RET:
		case ARM64_INS_CBZ:
		case ARM64_INS_CBNZ:
		case ARM64_INS_TBZ:
		case ARM64_INS_TBNZ:
			return true;
		default:
			return false;
		}
	}

	bool is_load_instruction(arm64_insn id) {
		switch (id) {
		case ARM64_INS_LDR:
		case ARM64_INS_LDRB:
		case ARM64_INS_LDRH:
		case ARM64_INS_LDRSW:
		case ARM64_INS_LDUR:
		case ARM64_INS_LDURB:
		case ARM64_INS_LDURH:
		case ARM64_INS_LDURSW:
		case ARM64_INS_LDP:
			return true;
		default:
			return false;
		}
	}

	bool writes_reg(const cs_insn* insn, arm64_reg reg) {
		if (!insn || !insn->detail) return false;
		reg = normalize_gp_reg(reg);
		const auto& arm64 = insn->detail->arm64;
		for (uint8_t i = 0; i < arm64.op_count; ++i) {
			const auto& op = arm64.operands[i];
			if (op.type == ARM64_OP_REG &&
				(op.access & CS_AC_WRITE) != 0 &&
				normalize_gp_reg(op.reg) == reg) {
				return true;
			}
		}
		return false;
	}

	bool get_mov_reg_alias(const cs_insn* insn, arm64_reg& dst, arm64_reg& src) {
		if (!insn || !insn->detail || insn->id != ARM64_INS_MOV) return false;
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count < 2 ||
			arm64.operands[0].type != ARM64_OP_REG ||
			arm64.operands[1].type != ARM64_OP_REG) {
			return false;
		}
		dst = normalize_gp_reg(arm64.operands[0].reg);
		src = normalize_gp_reg(arm64.operands[1].reg);
		return true;
	}

	bool get_stack_slot(const cs_insn* insn, uint8_t mem_op, int32_t& disp) {
		if (!insn || !insn->detail) return false;
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count <= mem_op || arm64.operands[mem_op].type != ARM64_OP_MEM ||
			arm64.operands[mem_op].mem.base != ARM64_REG_SP) {
			return false;
		}
		disp = arm64.operands[mem_op].mem.disp;
		return true;
	}

	bool get_single_stack_store(const cs_insn* insn, arm64_reg& src, int32_t& disp) {
		if (!insn || !insn->detail ||
			(insn->id != ARM64_INS_STR && insn->id != ARM64_INS_STUR)) {
			return false;
		}
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count < 2 || arm64.operands[0].type != ARM64_OP_REG ||
			!get_stack_slot(insn, 1, disp)) {
			return false;
		}
		src = normalize_gp_reg(arm64.operands[0].reg);
		return true;
	}

	bool get_single_stack_load(const cs_insn* insn, arm64_reg& dst, int32_t& disp) {
		if (!insn || !insn->detail ||
			(insn->id != ARM64_INS_LDR && insn->id != ARM64_INS_LDUR)) {
			return false;
		}
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count < 2 || arm64.operands[0].type != ARM64_OP_REG ||
			!get_stack_slot(insn, 1, disp)) {
			return false;
		}
		dst = normalize_gp_reg(arm64.operands[0].reg);
		return true;
	}

	bool get_pair_stack_store(const cs_insn* insn, arm64_reg& src0, arm64_reg& src1, int32_t& disp) {
		if (!insn || !insn->detail || insn->id != ARM64_INS_STP) return false;
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count < 3 ||
			arm64.operands[0].type != ARM64_OP_REG ||
			arm64.operands[1].type != ARM64_OP_REG ||
			!get_stack_slot(insn, 2, disp)) {
			return false;
		}
		src0 = normalize_gp_reg(arm64.operands[0].reg);
		src1 = normalize_gp_reg(arm64.operands[1].reg);
		return true;
	}

	bool get_pair_stack_load(const cs_insn* insn, arm64_reg& dst0, arm64_reg& dst1, int32_t& disp) {
		if (!insn || !insn->detail || insn->id != ARM64_INS_LDP) return false;
		const auto& arm64 = insn->detail->arm64;
		if (arm64.op_count < 3 ||
			arm64.operands[0].type != ARM64_OP_REG ||
			arm64.operands[1].type != ARM64_OP_REG ||
			!get_stack_slot(insn, 2, disp)) {
			return false;
		}
		dst0 = normalize_gp_reg(arm64.operands[0].reg);
		dst1 = normalize_gp_reg(arm64.operands[1].reg);
		return true;
	}

	bool is_tracked(const std::set<arm64_reg>& regs, arm64_reg reg) {
		return regs.find(normalize_gp_reg(reg)) != regs.end();
	}

	bool nearest_mov_immediate(method* entry, uint32_t index, arm64_reg reg,
		uint32_t lower_bound, uint32_t max_back, int64_t& value, uint32_t& def_index) {
		reg = normalize_gp_reg(reg);
		const uint32_t available = index > lower_bound ? index - lower_bound : 0;
		const uint32_t distance = available < max_back ? available : max_back;
		for (uint32_t step = 1; step <= distance; ++step) {
			const uint32_t current = index - step;
			const cs_insn* insn = entry->get_insn(current);
			if (is_control_flow(insn)) break;
			if (!writes_reg(insn, reg)) continue;

			const auto& arm64 = insn->detail->arm64;
			if (insn->id == ARM64_INS_MOV && arm64.op_count >= 2 &&
				arm64.operands[0].type == ARM64_OP_REG &&
				arm64.operands[1].type == ARM64_OP_IMM) {
				value = arm64.operands[1].imm;
				def_index = current;
				return true;
			}
			return false;
		}
		return false;
	}

	uint32_t find_pool_load(method* entry, uint32_t madd, uint32_t end,
		uint32_t max_forward, int32_t& disp) {
		std::set<arm64_reg> aliases{ normalize_gp_reg(entry->reg(madd, 0)) };
		std::unordered_map<int32_t, bool> stack_aliases;
		const uint32_t upper_bound = madd + max_forward < end ? madd + max_forward : end;

		for (uint32_t current = madd + 1; current <= upper_bound; ++current) {
			const cs_insn* insn = entry->get_insn(current);
			if (is_control_flow(insn)) break;

			if (is_load_instruction(static_cast<arm64_insn>(insn->id)) && insn->detail) {
				const auto& arm64 = insn->detail->arm64;
				for (uint8_t op_index = 0; op_index < arm64.op_count; ++op_index) {
					const auto& op = arm64.operands[op_index];
					if (op.type == ARM64_OP_MEM && is_tracked(aliases, op.mem.base)) {
						disp = op.mem.disp;
						return current;
					}
				}
			}

			std::vector<arm64_reg> add_aliases;
			std::vector<arm64_reg> remove_aliases;
			if (insn->detail) {
				const auto& arm64 = insn->detail->arm64;
				if (arm64.op_count >= 2 &&
					arm64.operands[0].type == ARM64_OP_REG &&
					(arm64.operands[0].access & CS_AC_WRITE) != 0) {
					for (uint8_t op_index = 1; op_index < arm64.op_count; ++op_index) {
						const auto& op = arm64.operands[op_index];
						if (op.type == ARM64_OP_REG && is_tracked(aliases, op.reg)) {
							add_aliases.push_back(normalize_gp_reg(arm64.operands[0].reg));
							break;
						}
					}
				}
			}
			arm64_reg dst = ARM64_REG_INVALID;
			arm64_reg src = ARM64_REG_INVALID;
			if (get_mov_reg_alias(insn, dst, src) && is_tracked(aliases, src)) {
				add_aliases.push_back(dst);
			}

			int32_t stack_disp = 0;
			if (get_single_stack_store(insn, src, stack_disp) && is_tracked(aliases, src)) {
				stack_aliases[stack_disp] = true;
			}
			arm64_reg src0 = ARM64_REG_INVALID;
			arm64_reg src1 = ARM64_REG_INVALID;
			if (get_pair_stack_store(insn, src0, src1, stack_disp)) {
				if (is_tracked(aliases, src0)) stack_aliases[stack_disp] = true;
				if (is_tracked(aliases, src1)) stack_aliases[stack_disp + 8] = true;
			}

			if (get_single_stack_load(insn, dst, stack_disp) && stack_aliases.find(stack_disp) != stack_aliases.end()) {
				add_aliases.push_back(dst);
			}
			arm64_reg dst0 = ARM64_REG_INVALID;
			arm64_reg dst1 = ARM64_REG_INVALID;
			if (get_pair_stack_load(insn, dst0, dst1, stack_disp)) {
				if (stack_aliases.find(stack_disp) != stack_aliases.end()) add_aliases.push_back(dst0);
				if (stack_aliases.find(stack_disp + 8) != stack_aliases.end()) add_aliases.push_back(dst1);
			}

			for (arm64_reg alias : aliases) {
				if (writes_reg(insn, alias)) remove_aliases.push_back(alias);
			}
			for (arm64_reg alias : remove_aliases) aliases.erase(alias);
			for (arm64_reg alias : add_aliases) aliases.insert(alias);

			if (aliases.empty() && stack_aliases.empty()) break;
		}
		return 0;
	}

	bool is_strict_subset(const std::set<std::string>& subset,
		const std::set<std::string>& superset) {
		return subset.size() < superset.size() &&
			std::includes(superset.begin(), superset.end(),
				subset.begin(), subset.end());
	}
}

namespace coord_dec {

	bool FindDec::find_v87_str() {
		uint32_t index = entry->find(
			[](finder& f) {
				f.is(0, ARM64_INS_MRS);
				f.is_reg(0, 1, ARM64_SYSREG_CTR_EL0);
			}
		);

		if (!index) {
			return false;
		}


		index = entry->find_reverse(
			[](finder& f) {
				f.is_str();
			}, index, 5
		);

		entry->add_point("v87_end", index, entry->reg(index, 0));
		v87_str_point = index;
		return true;
	}

	bool FindDec::find_binary_search_end() {
		std::vector<uint32_t> indexes;
		entry->find_all([](finder& f) {
			f.is(0, ARM64_INS_MADD, ARM64_INS_SMADDL);
			}, indexes);

		if (indexes.empty()) {
			return false;
		}

		uint32_t hash_end = 0;
		arm64_reg reg;
		for (auto index : indexes) {
			reg = entry->reg(index, 0);

			uint32_t mov = entry->find_reverse([&](finder& f) {
				f.is_mov();
				f.is_reg(0, 0, toW(entry->reg(index, 2)));
				f.is_imm(0, 1);
				f.break_when_b();
				f.break_when_has_reg(0, entry->reg(index, 2));
				}, index, 60);

			if (!mov) {
				continue;
			}

			uint32_t offset = entry->find([&](finder& f) {
				f.is(0, ARM64_INS_STP, ARM64_INS_STR);
				f.has_reg(0, reg);
				f.has_mem_base(0, ARM64_REG_SP);
				f.break_when_b();
				f.break_when_reg_is(0, 0, reg);
				}, index, 20);

			if (!offset) {
				continue;
			}


			hash_end_madd = index;
			hash_end = offset;

			if (entry->id(index) == ARM64_INS_MADD) {
				break;
			}

		}

		if (!hash_end) {
			return false;
		}

		entry->add_point("hash_end", hash_end, reg);
		return true;
	}

	bool FindDec::find_pool_ptr_offset() {
		uint64_t madd = hash_end_madd;

		if (!madd) {
			return false;
		}


		arm64_reg reg = entry->reg(madd, 3);
		uint32_t stp = madd;

		while (true) {

			uint32_t pool_ptr_calc_b = entry->find_reverse([&](finder& f) {
				f.is_b();
				}, stp, 100);

			if (!pool_ptr_calc_b) {
				return false;
			}


			Analyze pool_ptr_calc;
			for (uint32_t i = pool_ptr_calc_b + 1; i < stp; i++) {
				if (pool_ptr_calc.parse(entry->get_insn(i)) != 0) {
					return false;
				}
			}

			std::set<mem_param> pool_ptr_calc_params;
			int32_t disp = 0;
			if (!pool_ptr_calc.list_mem_param(reg, pool_ptr_calc_params)) {
				return false;
			}
			for (auto p : pool_ptr_calc_params) {

				if (p.disp == entry->mem_disp(v87_str_point, 1)) {
					if (p.offset.size() > 0) {
						pool_ptr_offset = p.offset[0];
					}
					else {
						std::unordered_map<std::string, uint64_t> params;
						params[p.name] = 0;
						pool_ptr_offset = pool_ptr_calc.execute(reg, params);
					}
					return true;
				}

				if (p.size == 8) {
					disp = p.disp;
					break;
				}
				else {
					continue;
				}
			}

			stp = entry->find([&](finder& f) {
				f.st(0, ARM64_REG_SP, disp);
				});

			if (!stp) {
				return false;
			}

			reg = entry->st_reg(stp, disp);

			if (reg == ARM64_REG_INVALID) {
				return false;
			}

		}


		return false;
	}

	bool FindDec::analyze_hash_binary_search() {
		failure_stage_ = FindDecFailureStage::HashSearch;
		if (!find_binary_search_end()) {
			return false;
		}
		failure_stage_ = FindDecFailureStage::PoolPointer;
		if (!find_pool_ptr_offset()) {
			return false;
		}
		return true;
	}

	bool FindDec::find_ring_offset() {

		std::vector<uint32_t> madd;
		entry->find_all(
			[](finder& f) {
				f.is(0, ARM64_INS_MADD, ARM64_INS_UMADDL);
			}, madd
		);

		uint32_t ring_madd = 0;
		for (uint32_t i : madd) {
			int64_t immediate = 0;
			uint32_t index = 0;
			if (!nearest_mov_immediate(entry, i, entry->reg(i, 2),
				entry->start_i(), kRingStrideSearchBack, immediate, index) ||
				immediate != 48) {
				continue;
			}



			ring_madd = i;
			break;
		}

		if (!ring_madd) {
			return false;
		}

		ring_offset_madd_block = entry->find_reverse([&](finder& f) {
			f.is_b();
			}, ring_madd, 100) + 1;

		uint32_t add = entry->find_reverse([&](finder& f) {
			f.is_add(0);
			f.is_reg(0, 0, entry->reg(ring_madd, 3));
			f.break_when_b();
			}, ring_madd, 50);

		if (!add) {
			ring_offset = 0;
		}
		else {
			ring_offset = entry->imm(add, 2);
		}


		return true;
	}

	bool FindDec::analyze_base_index_calc() {
		std::vector<uint32_t> madds;
		entry->find_all(ring_offset_madd_block, ring_offset_madd_block + 100,
			[](finder& f) {
				f.is(0, ARM64_INS_MADD, ARM64_INS_UMADDL);
				f.break_when_b();
			}, madds);
		madd_count_ = static_cast<uint16_t>(std::min<std::size_t>(
			madds.size(), std::numeric_limits<uint16_t>::max()));

		if (madds.empty()) {
			failure_detail_ = FindDecFailureDetail::MaddScanEmpty;
			return false;
		}

		uint32_t ring_base_madd = 0;
		uint32_t pool_ldr = 0;
		int32_t pool_disp = 0;

		std::vector<uint32_t> ring_madds;
		for (auto madd : madds) {
			int64_t immediate = 0;
			uint32_t immediate_def = 0;
			if (!nearest_mov_immediate(entry, madd, entry->reg(madd, 2),
				ring_offset_madd_block, kRingStrideSearchBack, immediate, immediate_def)) {
				continue;
			}

			if (immediate == 48) {
				ring_madds.push_back(madd);
				continue;
			}

			int32_t candidate_disp = 0;
			uint32_t candidate_ldr = find_pool_load(entry, madd,
				ring_offset_madd_block + 100, 50, candidate_disp);
			if (candidate_ldr && !ring_base_madd) {
				ring_base_madd = madd;
				pool_ldr = candidate_ldr;
				pool_disp = candidate_disp;
			}
		}
		ring_madd_count_ = static_cast<uint16_t>(std::min<std::size_t>(
			ring_madds.size(), std::numeric_limits<uint16_t>::max()));

		if (ring_madds.empty()) {
			failure_detail_ = FindDecFailureDetail::RingStrideMissing;
			return false;
		}

		if (ring_madds.size() != 2) {
			failure_detail_ = FindDecFailureDetail::RingStrideCount;
			return false;
		}

		if (!ring_base_madd || !pool_ldr) {
			failure_detail_ = FindDecFailureDetail::PoolLoadMissing;
			return false;
		}


		uint32_t end = ring_madds.back() - 1;
		uint32_t start = ring_offset_madd_block;

		entry->add_point("ring_calc_start", start);

		bool for_index = false;

		struct RingIndexCandidate {
			uint32_t madd = 0;
			std::shared_ptr<Expr> expr;
			std::set<mem_param> memory_params;
			std::set<std::string> dependencies;
		};

		uint32_t count = 0;
		std::vector<RingIndexCandidate> candidates;
		for (uint32_t i = start; i <= end; i++) {

			if (i == ring_base_madd) continue;

			if (entry->id(i) == ARM64_INS_BL) {

				auto decode = binary_.create_method("decode", entry->imm(i, 0), [](finder& f) {
					f.is_ret();
					}, 500
				);

				if (!decode) {
					continue;
				}

				for (uint32_t j = decode->start_i(); j < decode->end_i(); j++) {
					cs_insn* instruction = entry->get_insn(j);
					if (analyze.parse(instruction)) {
						failure_detail_ =
							FindDecFailureDetail::DecodeExpressionParse;
						failure_instruction_ = static_cast<uint16_t>(
							std::min<unsigned int>(
								instruction->id,
								std::numeric_limits<uint16_t>::max()));
						return false;
					}
				}
			}
			else {
				cs_insn* instruction = entry->get_insn(i);
				if (analyze.parse(instruction)) {
					failure_detail_ =
						FindDecFailureDetail::IndexExpressionParse;
					failure_instruction_ = static_cast<uint16_t>(
						std::min<unsigned int>(
							instruction->id,
							std::numeric_limits<uint16_t>::max()));
					return false;
				}
			}

			if (count < ring_madds.size() && i == ring_madds[count] - 1) {
				count++;
				auto candidate_expr = analyze.get_expr(entry->reg(i + 1, 1));
				if (!candidate_expr) {
					failure_detail_ =
						FindDecFailureDetail::CandidateExpressionMissing;
					return false;
				}

				RingIndexCandidate candidate;
				candidate.madd = i + 1;
				candidate.expr = candidate_expr;
				candidate.expr->param(candidate.memory_params);
				candidate.expr->dependencies(candidate.dependencies);
				candidates.push_back(std::move(candidate));
			}
		}
		candidate_count_ = static_cast<uint16_t>(std::min<std::size_t>(
			candidates.size(), std::numeric_limits<uint16_t>::max()));

		if (candidates.size() != 2) {
			failure_detail_ = FindDecFailureDetail::CandidateCount;
			return false;
		}

		auto uses_ring_index = [&](const RingIndexCandidate& candidate) {
			return std::any_of(candidate.memory_params.begin(), candidate.memory_params.end(),
				[&](const mem_param& p) {
					return !p.offset.empty() && p.offset[0] == pool_disp;
				});
		};

		const bool first_is_current = uses_ring_index(candidates[0]) &&
			is_strict_subset(candidates[0].dependencies, candidates[1].dependencies);
		const bool second_is_current = uses_ring_index(candidates[1]) &&
			is_strict_subset(candidates[1].dependencies, candidates[0].dependencies);
		if (first_is_current == second_is_current) {
			failure_detail_ =
				FindDecFailureDetail::CandidateDependencyAmbiguous;
			return false;
		}

		const RingIndexCandidate& current = candidates[first_is_current ? 0 : 1];
		index_expr = current.expr;
		analyze.retain_var_params(current.dependencies);

		entry->add_point("all_params_exec_end", current.madd);

		std::set<mem_param> params;
		index_expr->param(params);

		for (auto& p : params) {
			if (!for_index && !p.offset.empty() && p.offset[0] == pool_disp) {
				for_index = true;
				index_offset = p.offset[0];
				ring_index_param = p.name;
				continue;
			}
			if (p.runtime) {
				continue;
			}

			mem_param_list.push_back({ p.name, p.size, p.disp, 0, p.offset });
		}

		if (!for_index || ring_index_param.empty() || index_offset != pool_disp) {
			failure_detail_ = FindDecFailureDetail::IndexParameterMissing;
			return false;
		}

		failure_detail_ = FindDecFailureDetail::None;
		return true;
	}

	bool FindDec::analyze_index_calc() {
		failure_stage_ = FindDecFailureStage::RingOffset;
		if (!find_ring_offset()) {
			return false;
		}

		failure_stage_ = FindDecFailureStage::IndexExpression;
		if (!analyze_base_index_calc()) {
			return false;
		}
		return true;
	}

	uint32_t generate_branch(int32_t offset_in_bytes) {
		if (offset_in_bytes % 4 != 0) {
			return 0;
		}

		int32_t offset_in_instructions = offset_in_bytes / 4;

		const int32_t MIN_OFFSET = -(1 << 25);
		const int32_t MAX_OFFSET = (1 << 25) - 1;
		if (offset_in_instructions < MIN_OFFSET || offset_in_instructions > MAX_OFFSET) {
			return 0;
		}

		uint32_t imm26 = static_cast<uint32_t>(offset_in_instructions) & 0x03FFFFFF;

		return 0x14000000 | imm26;
	}

	bool FindDec::patch() {

		uint8_t patch_nop[] = { 0x1F, 0x20, 0x03, 0xD5 };

		std::vector<uint32_t> indexes;
		entry->find_all(
			[](finder& f) {
				f.is_blr();
			}, indexes
		);

		std::vector<uint32_t> patch_blr;
		for (uint32_t i : indexes) {
			uint32_t index = entry->find_reverse(
				[](finder& f) {
					f.is_ldr();
					f.is_reg(0, 0, ARM64_REG_W0);
				}, i, 5);

			if (index) {
				index = entry->find(
					[](finder& f) {
						f.is_str();
						f.is_reg(0, 0, ARM64_REG_X0);
						f.is_mem_base(0, 1, ARM64_REG_SP);
					}, i, 10);

				if (index) {
					continue;
				}

			}

			patch_blr.push_back(i);
		}

		for (const auto& i : patch_blr) {
			binary_.patch(i, patch_nop, 4);
		}

		std::vector<uint32_t> patch_svc;
		entry->find_all(
			[](finder& f) {
				f.is(0, ARM64_INS_SVC);
			}, patch_svc
		);

		indexes.clear();
		entry->find_all(
			[](finder& f) {
			f.is_str();
			f.is_reg(0, 0, ARM64_REG_XZR);
			}, indexes);

		if (indexes.empty()) {
			return false;
		}

		uint32_t str = 0;
		for (auto i : indexes) {

			uint64_t csel = entry->find([](finder& f) {
				f.is(0, ARM64_INS_CSEL);
				f.break_when_b();
				}, i, 50);

			if (!csel) {
				continue;
			}

			uint64_t mov = entry->find_reverse([&](finder& f) {
				f.imm(0, 1, 1);
				f.is(0, ARM64_INS_CMP);
				f.break_when_b();
				}, csel, 50);

			if (!mov) {
				continue;
			}

			str = i;

		}

		if (!str) {
			return false;
		}


		uint64_t addr = entry->find_reverse([](finder& f) {
			f.is_b();
			}, str, 20);

		if (!addr) return false;

		addr = entry->address(addr + 1);

		for (const auto& i : patch_svc) {
			uint32_t patch_b = generate_branch(addr - entry->address(i));
			binary_.patch(i, &patch_b, 4);
		}

		if (!patch_svc.empty()) {
			entry->add_point("jump_to_ring_calc", patch_svc[0]);
		}

		return true;
	}


	int FindDec::find_dec(uint64_t entry_address) {

		params.clear();
		analyze.reset();
		mem_param_list.clear();
		index_expr.reset();
		ring_index_param.clear();
		ring_offset = 0;
		v87_str_point = 0;
		ring_offset_madd_block = 0;
		hash_end_madd = 0;
		index_offset = 0;
		pool_ptr_offset = 0;
		failure_stage_ = FindDecFailureStage::EntryMethod;
		failure_detail_ = FindDecFailureDetail::None;
		madd_count_ = 0;
		ring_madd_count_ = 0;
		candidate_count_ = 0;
		failure_instruction_ = 0;


		finder f;
		f.is_ret(0);

		entry = binary_.create_method("entry", entry_address, f, 5000);
		if (!entry) {
			return -1;
		}


		failure_stage_ = FindDecFailureStage::V87Marker;
		if (!find_v87_str()) {
			return -1;
		}


		if (!analyze_hash_binary_search()) {
			return -1;
		}



		if (!analyze_index_calc()) {
			return -1;
		}
		failure_stage_ = FindDecFailureStage::Patch;
		if (!patch()) {
			return -1;
		}





		failure_stage_ = FindDecFailureStage::None;
		return 0;
	}
}

}  // namespace lengjing::game::native::coordinate_pool_internal
