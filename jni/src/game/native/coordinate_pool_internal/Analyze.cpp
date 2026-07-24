#include "game/native/coordinate_pool_internal/Analyze.h"
#include <algorithm>
#include <cstdint>
#include "game/native/coordinate_pool_internal/DecFormat.h"

namespace lengjing::game::native::coordinate_pool_internal {

static bool isW(arm64_reg reg) {
    return (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30) ||
        reg == ARM64_REG_WSP || reg == ARM64_REG_WZR;
}


static arm64_reg normalizeReg(arm64_reg reg) {
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

static bool isScalarReg(arm64_reg reg) {
    reg = normalizeReg(reg);
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
    reg = normalizeReg(reg);
    if (reg == ARM64_REG_XZR) {
        regs.erase(reg);
        return;
    }
    regs[reg] = std::make_shared<VarExpr>(name, 0x0);
}

uint64_t Analyze::execute(arm64_reg reg, std::unordered_map<std::string, uint64_t>& params) {
    return regs[normalizeReg(reg)]->eval(params);
}

std::shared_ptr<Expr> Analyze::get_expr(arm64_reg reg)
{
    reg = normalizeReg(reg);

    if (reg == ARM64_REG_XZR) {
        return make_const(0);
    }
    if (reg == ARM64_REG_SP || reg == ARM64_REG_X30) {
        const std::string name = reg == ARM64_REG_SP
            ? coordinate_pool_format::Format("SP_0x{:X}", cur_addr)
            : coordinate_pool_format::Format("X30_0x{:X}", cur_addr);
        const bool exists = std::any_of(
            varParams.begin(), varParams.end(),
            [&](const VarParam& param) {
                return param.name == name;
            });
        if (!exists) {
            varParams.push_back({name, cur_addr, reg});
        }
        return std::make_shared<VarExpr>(name, cur_addr);
    }
    auto it = regs.find(reg);
    if (it != regs.end()) {
        return it->second;
    }

    if (!((reg >= ARM64_REG_X0 && reg <= ARM64_REG_X28) ||
          reg == ARM64_REG_X29)) {
        return nullptr;
    }

    std::string name;
    if (reg == ARM64_REG_X29) {
        name = coordinate_pool_format::Format("X29_0x{:X}", cur_addr);
    }
    else {
        name = coordinate_pool_format::Format(
            "X{}_0x{:X}", reg - ARM64_REG_X0, cur_addr);
    }
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
    arm64_reg writeback_base = ARM64_REG_INVALID;
    if (insn->detail->arm64.writeback) {
        for (std::uint8_t i = 0; i < op_count; ++i) {
            if (op[i].type == ARM64_OP_MEM) {
                writeback_base = normalizeReg(op[i].mem.base);
                break;
            }
        }
    }
    if (op_count > 0 && op[0].type == ARM64_OP_REG &&
        !isScalarReg(op[0].reg)) {
        if (writeback_base != ARM64_REG_INVALID) {
            regs.erase(writeback_base);
        }
        return 0;
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
                  isW(operand.reg) && operand.shift.value >= 32) ||
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
            if (operand.type == ARM64_OP_REG && isW(operand.reg) &&
                operand.ext == ARM64_EXT_INVALID) {
                expr = sign_extend_expr(std::move(expr), 32);
            }
            shift_op = SHIFT_OP_ASR;
        }
        else {
            return nullptr;
        }
        return make_shift(shift_op, expr, operand.shift.value);
    };

    auto assign_result = [&](arm64_reg destination,
        std::shared_ptr<Expr> expr) {
        destination = normalizeReg(destination);
        if (destination == ARM64_REG_XZR) {
            regs.erase(destination);
            return;
        }
        if (destination == ARM64_REG_SP ||
            destination == ARM64_REG_X30) {
            regs.erase(destination);
            return;
        }
        if (expr) {
            regs[destination] = std::move(expr);
        }
        else {
            regs.erase(destination);
        }
    };

    auto store_result = [&](std::shared_ptr<Expr> expr) -> bool {
        if (!expr) {
            return false;
        }
        if (isW(op[0].reg)) {
            expr = make_binary(OP_AND, expr, make_const(0xFFFFFFFFULL));
        }
        assign_result(reg, std::move(expr));
        return true;
    };

    auto make_runtime_memory = [&](arm64_reg destination, uint32_t size,
        int32_t displacement, arm64_reg base) {
        const arm64_reg normalized_destination = normalizeReg(destination);
        int destination_index = 31;
        if (normalized_destination >= ARM64_REG_X0 &&
            normalized_destination <= ARM64_REG_X28) {
            destination_index =
                normalized_destination - ARM64_REG_X0;
        }
        else if (normalized_destination == ARM64_REG_X29) {
            destination_index = 29;
        }
        else if (normalized_destination == ARM64_REG_X30) {
            destination_index = 30;
        }
        const std::string name = coordinate_pool_format::Format(
            "MEM_X{}_0x{:X}",
            destination_index,
            insn->address);
        const std::string base_name = base == ARM64_REG_SP
            ? "SP"
            : coordinate_pool_format::Format(
                "X{}", normalizeReg(base) - ARM64_REG_X0);
        assign_result(
            destination,
            make_mem(name, size, 0, base_name)->to_runtime(
                size, displacement));
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
            assign_result(reg, std::move(expr));
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
                    assign_result(
                        reg,
                        make_mem(mem, size, op[1].mem.disp, "SP"));
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

            assign_result(reg, var->to(size, op[1].mem.disp));
            break;
        }
        case ARM64_INS_LDP: {
            if (op[2].mem.base == ARM64_REG_SP) {
                int32_t ld_1 = op[2].mem.disp;
                int32_t ld_2 = op[2].mem.disp + 8;
                std::string mem_1 = coordinate_pool_format::Format("(SP+0x{:X})", ld_1);
                std::string mem_2 = coordinate_pool_format::Format("(SP+0x{:X})", ld_2);
                assign_result(
                    op[0].reg, make_mem(mem_1, 8, ld_1, "SP"));
                assign_result(
                    op[1].reg, make_mem(mem_2, 8, ld_2, "SP"));
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
    if (writeback_base != ARM64_REG_INVALID) {
        regs.erase(writeback_base);
    }
    return 0;
}

}  // namespace lengjing::game::native::coordinate_pool_internal
