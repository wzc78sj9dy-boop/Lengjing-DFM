#include "game/native/coordinate_pool_internal/Analyze.h"
#include <algorithm>
#include <cstdint>
#include "game/native/coordinate_pool_internal/DecFormat.h"

namespace lengjing::game::native::coordinate_pool_internal {

static bool isW(arm64_reg reg) {
    return reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30;
}


static arm64_reg normalizeReg(arm64_reg reg) {
    return isW(reg) ? (arm64_reg) (reg - ARM64_REG_W0 + ARM64_REG_X0) : reg;
}

static bool isCompatW(arm64_reg reg) {
    return (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30) ||
        reg == ARM64_REG_WSP || reg == ARM64_REG_WZR;
}

static arm64_reg normalizeCompatReg(arm64_reg reg) {
    if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W28) {
        return static_cast<arm64_reg>(
            reg - ARM64_REG_W0 + ARM64_REG_X0);
    }
    if (reg == ARM64_REG_W29) return ARM64_REG_X29;
    if (reg == ARM64_REG_W30) return ARM64_REG_X30;
    if (reg == ARM64_REG_WSP) return ARM64_REG_SP;
    if (reg == ARM64_REG_WZR) return ARM64_REG_XZR;
    return reg;
}

static bool isCompatScalarReg(arm64_reg reg) {
    reg = normalizeCompatReg(reg);
    return (reg >= ARM64_REG_X0 && reg <= ARM64_REG_X28) ||
        reg == ARM64_REG_X29 || reg == ARM64_REG_X30 ||
        reg == ARM64_REG_SP || reg == ARM64_REG_XZR;
}

void Analyze::str(arm64_reg reg, std::vector<std::string>& expr) {
    uint32_t var_number = 0;
    std::unordered_map<std::shared_ptr<Expr>, uint32_t> exprs;
    list_expr(reg, exprs);

    for (auto expr : exprs) {
        if (expr.second < 5 || !expr.first ||
            expr.first->kind() != EXPR_BINARY) continue;
        expr.first->str_name = coordinate_pool_format::Format("a{}", ++var_number);
    }

    expr.push_back(regs[normalizeReg(reg)]->str(expr));
}

std::string Analyze::str(arm64_reg reg) {
    std::vector<std::string> expr;
    auto it = regs.find(normalizeReg(reg));
    if (it == regs.end()) return "[null]";
    return it->second->str(expr);
}

void Analyze::setVal(arm64_reg reg, const char *name) {
    regs[normalizeReg(reg)] = std::make_shared<VarExpr>(name, 0x0);
}

uint64_t Analyze::execute(arm64_reg reg, std::unordered_map<std::string, uint64_t>& params) {
    return regs[normalizeReg(reg)]->eval(params);
}

std::shared_ptr<Expr> Analyze::get_expr(arm64_reg reg)
{
    reg = normalizeReg(reg);

    auto it = regs.find(reg);
    if (it != regs.end()) {
        return it->second;
    }

    if (reg < ARM64_REG_X0 || reg > ARM64_REG_X28) {
        return nullptr;
    }

    std::string name = coordinate_pool_format::Format(
        "X{}_0x{:X}", reg - ARM64_REG_X0, cur_addr);
    varParams.push_back({ name , cur_addr, reg });

    auto expr = std::make_shared<VarExpr>(name, cur_addr);
    regs[reg] = expr;

    return expr;
}

void Analyze::retain_var_params(const std::set<std::string>& dependencies) {
    varParams.erase(
        std::remove_if(varParams.begin(), varParams.end(),
            [&](const VarParam& param) {
                return dependencies.find(param.name) == dependencies.end();
            }),
        varParams.end());
}

std::shared_ptr<ConstExpr> Analyze::make_const(uint64_t v) {
    return std::make_shared<ConstExpr>(v, cur_addr);
}

std::shared_ptr<BinaryExpr> Analyze::make_binary(BinOp o,
    std::shared_ptr<Expr> a,
    std::shared_ptr<Expr> b) {
    if (!a || !b) {
        return nullptr;
    }
    return std::make_shared<BinaryExpr>(o, a, b, cur_addr);
}


std::shared_ptr<ShiftExpr> Analyze::make_shift(ShiftOp o,
    std::shared_ptr<Expr> e,
    int amt) {
    if (!e) {
        return nullptr;
    }
    return std::make_shared<ShiftExpr>(o, e, amt, cur_addr);
}

std::shared_ptr<UnaryExpr> Analyze::make_unary(UnaryOp o, std::shared_ptr<Expr> e) {
    if (!e) {
        return nullptr;
    }
    return std::make_shared<UnaryExpr>(o, e, cur_addr);
}

std::shared_ptr<MemVarExpr> Analyze::make_mem(std::string n, uint32_t size, int32_t disp, std::string base) {
    return std::make_shared<MemVarExpr>(std::move(n), size, disp, cur_addr, std::move(base));
}


int Analyze::parse(cs_insn *insn) {
    if (!insn || !insn->detail) {
        return -1;
    }

    cur_addr = insn->address;
    cs_arm64_op *op = insn->detail->arm64.operands;
    const uint8_t op_count = insn->detail->arm64.op_count;
    arm64_reg reg = ARM64_REG_INVALID;
    if (op_count > 0 && op[0].type == ARM64_OP_REG) {
        reg = normalizeReg(op[0].reg);
    }

    auto add_runtime_param = [&](const std::shared_ptr<Expr>& expr,
        arm64_reg source_reg) {
        if (!expr || expr->kind() != EXPR_MEMORY) {
            return;
        }
        auto *memory = static_cast<MemVarExpr *>(expr.get());
        if (!memory->runtime_) return;

        const bool exists = std::any_of(varParams.begin(), varParams.end(),
            [&](const VarParam& param) {
                return param.name == memory->name;
            });
        if (!exists) {
            varParams.push_back({ memory->name, cur_addr,
                normalizeReg(source_reg), 0 });
        }
    };

    auto operand_expr = [&](const cs_arm64_op &operand) -> std::shared_ptr<Expr> {
        std::shared_ptr<Expr> expr;
        if (operand.type == ARM64_OP_IMM) {
            expr = make_const(static_cast<uint64_t>(operand.imm));
        }
        else if (operand.type == ARM64_OP_REG) {
            expr = get_expr(operand.reg);
			add_runtime_param(expr, operand.reg);
            if (expr && isW(operand.reg)) {
                expr = make_binary(OP_AND, expr, make_const(0xFFFFFFFFULL));
            }
        }
        else {
            return nullptr;
        }

        if (!expr || operand.shift.value == 0) {
            return expr;
        }

        ShiftOp shift_op;
        if (operand.shift.type == ARM64_SFT_LSL) {
            shift_op = SHIFT_OP_LSL;
        }
        else if (operand.shift.type == ARM64_SFT_LSR) {
            shift_op = SHIFT_OP_LSR;
        }
        else {
            return nullptr;
        }
        return make_shift(shift_op, expr, operand.shift.value);
    };

    auto store_result = [&](std::shared_ptr<Expr> expr) -> bool {
        if (!expr) {
            return false;
        }
        if (isW(op[0].reg)) {
            expr = make_binary(OP_AND, expr, make_const(0xFFFFFFFFULL));
        }
        regs[reg] = std::move(expr);
        return true;
    };

    auto make_runtime_memory = [&](arm64_reg destination, uint32_t size,
        int32_t displacement, arm64_reg base) {
        const std::string name = coordinate_pool_format::Format(
            "MEM_X{}_0x{:X}",
            normalizeReg(destination) - ARM64_REG_X0,
            insn->address);
        const std::string base_name = base == ARM64_REG_SP
            ? "SP"
            : coordinate_pool_format::Format(
                "X{}", normalizeReg(base) - ARM64_REG_X0);
        regs[normalizeReg(destination)] =
            make_mem(name, size, 0, base_name)->to_runtime(
                size, displacement);
    };

    switch (insn->id) {
        case ARM64_INS_MOV: {
            auto expr = operand_expr(op[1]);
            if (!expr) {
                return -1;
            }
            if (isW(op[0].reg) && op[1].type == ARM64_OP_IMM) {
                expr = make_const(
                    static_cast<std::uint64_t>(op[1].imm) &
                    UINT64_C(0xFFFFFFFF));
            }
            regs[reg] = std::move(expr);
            break;
        }
        case ARM64_INS_MOVK: {
            if (op_count < 2 || op[1].type != ARM64_OP_IMM ||
                op[1].shift.value > 48 ||
                (op[1].shift.value & 15U) != 0) {
                return -1;
            }
            const unsigned int shift = op[1].shift.value;
            const std::uint64_t fieldMask = UINT64_C(0xFFFF) << shift;
            const std::uint64_t inserted =
                (static_cast<std::uint64_t>(op[1].imm) & UINT64_C(0xFFFF))
                << shift;
            const std::shared_ptr<Expr> previous = get_expr(op[0].reg);
            if (!store_result(make_binary(
                    OP_OR,
                    make_binary(
                        OP_AND,
                        previous,
                        make_const(~fieldMask)),
                    make_const(inserted)))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_ADD: {
            if (!store_result(make_binary(OP_ADD, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_EOR: {
            if (!store_result(make_binary(OP_XOR, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_SUB: {
            if (!store_result(make_binary(OP_SUB, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_LSR: {
            if (!store_result(make_binary(OP_LSR, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_LSL: {
            if (!store_result(make_binary(OP_LSL, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_AND: {
            if (!store_result(make_binary(OP_AND, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_UMULL:
        case ARM64_INS_MUL: {
            if (!store_result(make_binary(OP_MUL, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_ORR: {
            if (!store_result(make_binary(OP_OR, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_ORN: {
            if (!store_result(make_binary(OP_ORN, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_BIC: {
            if (!store_result(make_binary(OP_BIC, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_MVN: {
            if (!store_result(make_unary(OP_MVN, operand_expr(op[1])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_NEG: {
            if (!store_result(make_unary(OP_NEG, operand_expr(op[1])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_UMADDL:
        case ARM64_INS_MADD: {
            if (!store_result(make_binary(
                    OP_ADD,
                    operand_expr(op[3]),
                    make_binary(OP_MUL, operand_expr(op[1]), operand_expr(op[2]))))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_MSUB:
        case ARM64_INS_UMSUBL: {
            if (!store_result(make_binary(
                    OP_SUB,
                    operand_expr(op[3]),
                    make_binary(OP_MUL, operand_expr(op[1]), operand_expr(op[2]))))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_UMULH: {
            if (!store_result(make_binary(OP_UMULH, operand_expr(op[1]), operand_expr(op[2])))) {
                return -1;
            }
            break;
        }
        case ARM64_INS_LDRB:
        case ARM64_INS_LDR: {
            uint32_t size = insn->id == ARM64_INS_LDRB ? 1 : isW(op[0].reg) ? 4 : 8;

            auto it = regs.find(op[1].mem.base);
            if (it == regs.end()) {
                if (op[1].mem.base == ARM64_REG_SP) {
                    std::string mem = coordinate_pool_format::Format("(SP+0x{:X})", op[1].mem.disp);
                    regs[reg] = make_mem(mem, size, op[1].mem.disp, "SP");
                    break;
                } else if (normalizeReg(op[1].mem.base) >= ARM64_REG_X0 &&
					normalizeReg(op[1].mem.base) <= ARM64_REG_X28) {
					make_runtime_memory(
						op[0].reg, size, op[1].mem.disp, op[1].mem.base);
                    break;
                }
				regs.erase(reg);
                break;
            }

            if (!it->second || it->second->kind() != EXPR_MEMORY) {
				make_runtime_memory(
					op[0].reg, size, op[1].mem.disp, op[1].mem.base);
                break;
            }
            auto *var = static_cast<MemVarExpr *>(it->second.get());

            if (insn->detail->arm64.writeback) {
                if (regs.find(op[1].mem.base) == regs.end()) {
                    break;
                }
                if (!it->second || it->second->kind() != EXPR_MEMORY) {
                    break;
                }
                auto *back = static_cast<MemVarExpr *>(it->second.get());

                back->offset += op[1].mem.disp;
            }

            regs[reg] = var->to(size, op[1].mem.disp);
            break;
        }
        case ARM64_INS_LDP: {
            if (op[2].mem.base == ARM64_REG_SP) {
                int32_t ld_1 = op[2].mem.disp;
                int32_t ld_2 = op[2].mem.disp + 8;
                std::string mem_1 = coordinate_pool_format::Format("(SP+0x{:X})", ld_1);
                std::string mem_2 = coordinate_pool_format::Format("(SP+0x{:X})", ld_2);
                regs[normalizeReg(op[0].reg)] = make_mem(mem_1, 8, ld_1, "SP");
                regs[normalizeReg(op[1].reg)] = make_mem(mem_2, 8, ld_2, "SP");
                break;
            }
            regs.erase(normalizeReg(op[0].reg));
            regs.erase(normalizeReg(op[1].reg));
            break;
        }
        default:
            if (reg != ARM64_REG_INVALID &&
                (op[0].access & CS_AC_WRITE) != 0) {
                regs.erase(reg);
            }
            break;
    }
    return 0;
}

int Analyze::parse_add_compat(cs_insn *insn) {
    if (!insn || !insn->detail || insn->id != ARM64_INS_ADD) {
        return -1;
    }

    cur_addr = insn->address;
    cs_arm64_op *op = insn->detail->arm64.operands;
    const uint8_t op_count = insn->detail->arm64.op_count;
    if (op_count < 3 || op[0].type != ARM64_OP_REG) {
        return -1;
    }
    if (!isCompatScalarReg(op[0].reg)) {
        return 0;
    }

    auto truncate_expr = [&](std::shared_ptr<Expr> expr,
        unsigned int width) -> std::shared_ptr<Expr> {
        if (!expr || width == 0 || width > 64) return nullptr;
        if (width == 64) return expr;
        return make_binary(
            OP_AND, std::move(expr),
            make_const((UINT64_C(1) << width) - 1));
    };

    auto sign_extend_expr = [&](std::shared_ptr<Expr> expr,
        unsigned int width) -> std::shared_ptr<Expr> {
        expr = truncate_expr(std::move(expr), width);
        if (!expr || width == 0 || width > 64) return nullptr;
        if (width == 64) return expr;
        const std::uint64_t sign = UINT64_C(1) << (width - 1);
        return make_binary(
            OP_SUB,
            make_binary(OP_XOR, std::move(expr), make_const(sign)),
            make_const(sign));
    };

    auto runtime_expr = [&](arm64_reg source) -> std::shared_ptr<Expr> {
        const arm64_reg normalized = normalizeCompatReg(source);
        const char *prefix = normalized == ARM64_REG_SP
            ? "SP"
            : normalized == ARM64_REG_X30 ? "X30" : "X29";
        const std::string name = coordinate_pool_format::Format(
            "{}_0x{:X}", prefix, cur_addr);
        const bool exists = std::any_of(
            varParams.begin(), varParams.end(),
            [&](const VarParam& param) {
                return param.name == name;
            });
        if (!exists) {
            varParams.push_back({name, cur_addr, normalized, 0});
        }
        return std::make_shared<VarExpr>(name, cur_addr);
    };

    auto source_expr = [&](arm64_reg source) -> std::shared_ptr<Expr> {
        const arm64_reg normalized = normalizeCompatReg(source);
        if (normalized == ARM64_REG_XZR) {
            return make_const(0);
        }
        if (normalized == ARM64_REG_SP || normalized == ARM64_REG_X30) {
            return runtime_expr(source);
        }

        auto it = regs.find(normalized);
        if (it != regs.end()) {
            return it->second;
        }
        if (source == ARM64_REG_W29 || source == ARM64_REG_W30) {
            const arm64_reg legacy = normalizeReg(source);
            it = regs.find(legacy);
            if (it != regs.end()) {
                return it->second;
            }
        }
        if (normalized == ARM64_REG_X29) {
            return runtime_expr(source);
        }
        if (normalized >= ARM64_REG_X0 && normalized <= ARM64_REG_X28) {
            return get_expr(normalized);
        }
        return nullptr;
    };

    auto add_runtime_param = [&](const std::shared_ptr<Expr>& expr,
        arm64_reg source) {
        if (!expr || expr->kind() != EXPR_MEMORY) return;
        auto *memory = static_cast<MemVarExpr *>(expr.get());
        if (!memory->runtime_) return;
        const bool exists = std::any_of(
            varParams.begin(), varParams.end(),
            [&](const VarParam& param) {
                return param.name == memory->name;
            });
        if (!exists) {
            varParams.push_back({
                memory->name, cur_addr, normalizeCompatReg(source), 0});
        }
    };

    auto operand_expr = [&](const cs_arm64_op& operand)
        -> std::shared_ptr<Expr> {
        std::shared_ptr<Expr> expr;
        if (operand.type == ARM64_OP_IMM) {
            expr = make_const(static_cast<std::uint64_t>(operand.imm));
        }
        else if (operand.type == ARM64_OP_REG) {
            if (!isCompatScalarReg(operand.reg)) return nullptr;
            expr = source_expr(operand.reg);
            add_runtime_param(expr, operand.reg);
            if (expr && isCompatW(operand.reg)) {
                expr = truncate_expr(std::move(expr), 32);
            }
        }
        else {
            return nullptr;
        }

        switch (operand.ext) {
            case ARM64_EXT_INVALID:
            case ARM64_EXT_UXTX:
                break;
            case ARM64_EXT_UXTB:
                expr = truncate_expr(std::move(expr), 8);
                break;
            case ARM64_EXT_UXTH:
                expr = truncate_expr(std::move(expr), 16);
                break;
            case ARM64_EXT_UXTW:
                expr = truncate_expr(std::move(expr), 32);
                break;
            case ARM64_EXT_SXTB:
                expr = sign_extend_expr(std::move(expr), 8);
                break;
            case ARM64_EXT_SXTH:
                expr = sign_extend_expr(std::move(expr), 16);
                break;
            case ARM64_EXT_SXTW:
                expr = sign_extend_expr(std::move(expr), 32);
                break;
            case ARM64_EXT_SXTX:
                expr = sign_extend_expr(std::move(expr), 64);
                break;
            default:
                return nullptr;
        }

        if (!expr || operand.shift.value == 0) {
            return expr;
        }
        if (operand.ext != ARM64_EXT_INVALID) {
            if (operand.shift.type != ARM64_SFT_LSL ||
                operand.shift.value > 4) {
                return nullptr;
            }
        }
        else if ((operand.type == ARM64_OP_REG &&
                  isCompatW(operand.reg) && operand.shift.value >= 32) ||
                 operand.shift.value >= 64) {
            return nullptr;
        }

        ShiftOp shift_op;
        if (operand.shift.type == ARM64_SFT_LSL) {
            shift_op = SHIFT_OP_LSL;
        }
        else if (operand.shift.type == ARM64_SFT_LSR) {
            shift_op = SHIFT_OP_LSR;
        }
        else if (operand.shift.type == ARM64_SFT_ASR) {
            if (operand.type == ARM64_OP_REG &&
                isCompatW(operand.reg) &&
                operand.ext == ARM64_EXT_INVALID) {
                expr = sign_extend_expr(std::move(expr), 32);
            }
            shift_op = SHIFT_OP_ASR;
        }
        else {
            return nullptr;
        }
        return make_shift(shift_op, std::move(expr), operand.shift.value);
    };

    std::shared_ptr<Expr> lhs = operand_expr(op[1]);
    std::shared_ptr<Expr> rhs = operand_expr(op[2]);
    std::shared_ptr<Expr> result = make_binary(
        OP_ADD, std::move(lhs), std::move(rhs));
    if (!result) {
        return -1;
    }
    if (isCompatW(op[0].reg)) {
        result = truncate_expr(std::move(result), 32);
    }

    const arm64_reg destination = normalizeCompatReg(op[0].reg);
    if (destination == ARM64_REG_XZR ||
        destination == ARM64_REG_SP ||
        destination == ARM64_REG_X30) {
        return 0;
    }
    regs[destination] = std::move(result);
    return 0;
}

}  // namespace lengjing::game::native::coordinate_pool_internal
