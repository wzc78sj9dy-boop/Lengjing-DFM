#pragma once
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include "capstone/capstone.h"
#include <functional>
#include <limits>
#include <string>
#include "DecFormat.h"

namespace lengjing::game::native::coordinate_pool_internal {

    enum FIND_TYPE {
        MNEMONIC = 0,
        HAS_REG,
        INDEX_REG,
        MEM_BASE,
        MEM_DISP,
        IMM,
        OP,
        ST,
        LD,
        ANY,
    };

    struct stack {
        arm64_reg base;
        int disp;
    };

    class found {
        FIND_TYPE type;
        std::vector<arm64_insn> mnemonic_;
        std::vector<int> reg_;
        std::vector<int64_t> imm_;
        std::vector<int32_t> disp_;
        std::vector<stack> stack_;
        std::function<bool(const cs_insn& insn)> any_;
        int op_index_ = -1;
        std::string op_;
    public:
        found() {}

        found(arm64_insn mnemonic) {
            mnemonic_.push_back(mnemonic);
            type = MNEMONIC;
        }

        found(arm64_reg reg) {
            reg_.push_back(reg);
            type = HAS_REG;
        }

        found(std::vector<arm64_insn> mnemonic)
            : mnemonic_(std::move(mnemonic)), type(MNEMONIC) {
        }

        found(std::vector<int> reg) : reg_(std::move(reg)), type(HAS_REG) {}

        found(int reg_index, std::vector<int> reg, FIND_TYPE t)
            : op_index_(reg_index), reg_(std::move(reg)), type(t) {
        }

        found(int opt_index, std::vector<int32_t> disp)
            : op_index_(opt_index), disp_(std::move(disp)), type(MEM_DISP) {
        }

        found(int imm_index, std::vector<int64_t> imm)
            : op_index_(imm_index), imm_(std::move(imm)), type(IMM) {
        }

        found(std::function<bool(const cs_insn&)> any)
            : any_(std::move(any)), type(ANY) {
        }

        found(std::vector<stack> stack, FIND_TYPE t)
            : stack_(std::move(stack)), type(t) {
        }

        template<typename... Args>
        static found find_mnemonic(Args &&... args) {
            std::vector<arm64_insn> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { std::move(v) };
        }

        template<typename... Args>
        static found find_reg(Args &&... args) {
            std::vector<int> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { std::move(v) };
        }

        template<typename... Args>
        static found find_reg(int reg_index, Args &&... args) {
            std::vector<int> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { reg_index, std::move(v), INDEX_REG };
        }

        template<typename... Args>
        static found find_mem_base(int opt_index, Args &&... args) {
            std::vector<int> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { opt_index, std::move(v), MEM_BASE };
        }

        template<typename... Args>
        static found find_mem_disp(int opt_index, Args &&... args) {
            std::vector<int32_t> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { opt_index, std::move(v) };
        }

        template<typename... Args>
        static found find_imm(int opt_index, Args &&... args) {
            std::vector<int64_t> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { opt_index, std::move(v) };
        }

        template<typename... Args>
        static found find_st(Args &&... args) {
            std::vector<stack> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { std::move(v), ST };
        }

        template<typename... Args>
        static found find_ld(Args &&... args) {
            std::vector<stack> v;
            (v.push_back(std::forward<Args>(args)), ...);
            return { std::move(v), LD };
        }

        static found find_op(std::string op) {
            found f;
            f.op_ = std::move(op);
            f.type = OP;
            return f;
        }

        static found find_any(std::function<bool(const cs_insn& insn)> any) {
            return { std::move(any) };
        }

        bool find(const cs_insn& insn) {
            switch (type) {
            case MNEMONIC:
                return mnemonic(insn);
            case HAS_REG:
                return reg(insn);
            case INDEX_REG:
                return reg_index(insn);
            case MEM_BASE:
                return mem_base(insn);
            case MEM_DISP:
                return mem_disp(insn);
            case IMM:
                return imm(insn);
            case OP:
                return op(insn);
            case ST:
                return st(insn);
            case LD:
                return ld(insn);
            case ANY:
                return any_(insn);
            }
            return false;
        }

        bool mnemonic(const cs_insn& insn) {
            for (const auto& mnemonic : mnemonic_) {
                if (insn.id == mnemonic) {
                    return true;
                }
            }
            return false;
        }

        bool reg_index(const cs_insn& insn) {
            arm64_op_type op = insn.detail->arm64.operands[op_index_].type;
            if (op_index_ < insn.detail->arm64.op_count &&
                (op == ARM64_OP_REG || op == ARM64_OP_SYS)) {
                for (const auto& reg : reg_) {
                    if (reg == insn.detail->arm64.operands[op_index_].reg) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool reg(const cs_insn& insn) {
            for (uint8_t i = 0; i < insn.detail->arm64.op_count; ++i) {
                arm64_op_type op = insn.detail->arm64.operands[i].type;
                if ((op == ARM64_OP_REG || op == ARM64_OP_SYS)) {
                    for (const auto& reg : reg_) {
                        if (insn.detail->arm64.operands[i].reg == reg) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        bool mem_base(const cs_insn& insn) {
            if (op_index_ < insn.detail->arm64.op_count &&
                insn.detail->arm64.operands[op_index_].type == ARM64_OP_MEM) {
                for (const auto& base : reg_) {
                    if (base == insn.detail->arm64.operands[op_index_].mem.base) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool mem_disp(const cs_insn& insn) {
            if (op_index_ < insn.detail->arm64.op_count &&
                insn.detail->arm64.operands[op_index_].type == ARM64_OP_MEM) {
                for (const auto& disp : disp_) {
                    if (disp == insn.detail->arm64.operands[op_index_].mem.disp) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool imm(const cs_insn& insn) {
            if (op_index_ < insn.detail->arm64.op_count &&
                insn.detail->arm64.operands[op_index_].type == ARM64_OP_IMM) {
                for (const auto& imm : imm_) {
                    if (imm == insn.detail->arm64.operands[op_index_].imm) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool op(const cs_insn& insn) {
            if (strstr(insn.op_str, op_.c_str())) {
                return true;
            }
            return false;
        }

        bool st(const cs_insn& insn) {

            int i;
            if (insn.id == ARM64_INS_STR) i = 1;
            else if (insn.id == ARM64_INS_STP) i = 2;
            else return false;

            arm64_op_mem* op = &insn.detail->arm64.operands[i].mem;

            for (stack &stack: stack_) {
                if (stack.base == op->base && ((stack.disp == op->disp) || (i == 2 && stack.disp - 8 == op->disp))) {
                    return true;
                }
            }

            return false;
        }

        bool ld(const cs_insn& insn) {

            int i;
            if (insn.id == ARM64_INS_LDR) i = 1;
            else if (insn.id == ARM64_INS_LDP) i = 2;
            else return false;

            arm64_op_mem* op = &insn.detail->arm64.operands[i].mem;

            for (stack& stack : stack_) {
                if (stack.base == op->base && ((stack.disp == op->disp) || (i == 2 && stack.disp - 8 == op->disp))) {
                    return true;
                }
            }

            return false;
        }
    };

    struct found_ {
        uint32_t offset;
        found pattern;

        found_() = default;
        found_(uint32_t offset_, const found& value)
            : offset(offset_), pattern(value) {
        }
        found_(uint32_t offset_, found&& value)
            : offset(offset_), pattern(std::move(value)) {
        }
    };

    class finder {
        std::vector<found_> founds;
        std::vector<found_> break_;
        uint32_t range_;
        uint32_t limit_ = std::numeric_limits<uint32_t>::max();

        bool valid(uint32_t pos, uint32_t offset) const noexcept {
            return static_cast<uint64_t>(pos) + offset < limit_;
        }
    public:

        void limit(uint32_t index_limit) noexcept {
            limit_ = index_limit;
        }

        uint32_t find_range(uint32_t pos, cs_insn* insn) {
            return find_range(pos, range_, insn);
        }

        uint32_t find_range(uint32_t pos, uint32_t range, cs_insn* insn) {
            const uint64_t end = std::min<uint64_t>(
                static_cast<uint64_t>(limit_),
                static_cast<uint64_t>(pos) + range);
            for (uint32_t i = pos; static_cast<uint64_t>(i) < end; i++) {
                if (find(i, insn)) {
                    return i;
                }
                if (find_break(i, insn)) {
                    break;
                }
            }
            return 0;
        }

        uint32_t find_range_reverse(uint32_t pos, uint32_t range, cs_insn* insn) {
            if (limit_ == 0) return 0;
            if (pos >= limit_) pos = limit_ - 1;
            const uint32_t begin = pos < range ? 0 : pos - range;
            for (uint32_t i = pos;; i--) {
                if (find(i, insn)) {
                    return i;
                }
                if (find_break(i, insn)) {
                    break;
                }
                if (i == begin) break;
            }
            return 0;
        }

        bool find_break(uint32_t pos, cs_insn* insn) {
            for (found_& f : break_) {
                if (!valid(pos, f.offset)) continue;
                if (f.pattern.find(insn[pos + f.offset])) {
                    return true;
                }
            }
            return false;
        }

        bool find(uint32_t pos, cs_insn* insn) {
            for (found_& f : founds) {
                if (!valid(pos, f.offset)) return false;
                if (!f.pattern.find(insn[pos + f.offset])) {
                    return false;
                }
            }
            return true;
        }

        void range(uint32_t index_range) {
            range_ = index_range;
        }

        void add(uint32_t offset, const found& f) {
            founds.emplace_back(offset, f);
        }

        void add(const found& f) {
            founds.emplace_back(0x0, f);
        }

        void write_back(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_any([](const cs_insn& insn) {
                return insn.detail->arm64.writeback;
                }));
        }

        void is_imm(uint32_t offset, uint8_t op_index) {
            founds.emplace_back(offset, found::find_any([op_index](const cs_insn& insn) {
                if (insn.detail->arm64.op_count <= op_index) return false;
                return insn.detail->arm64.operands[op_index].type == ARM64_OP_IMM;
                }));
        }

        void has_mem_base(uint32_t offset, arm64_reg reg) {
            founds.emplace_back(offset, found::find_any([reg](const cs_insn& insn) {
                for (int i = 0; i < insn.detail->arm64.op_count; i++) {
                    if (insn.detail->arm64.operands[i].type == ARM64_OP_MEM) {
                        if (reg == insn.detail->arm64.operands[i].mem.base) {
                            return true;
                        }
                    }
                }
                return false;
                }));
        }

        void has_mem_disp(uint32_t offset, int32_t disp) {
            founds.emplace_back(offset, found::find_any([disp](const cs_insn& insn) {
                for (int i = 0; i < insn.detail->arm64.op_count; i++) {
                    if (insn.detail->arm64.operands[i].type == ARM64_OP_MEM) {
                        if (disp == insn.detail->arm64.operands[i].mem.disp) {
                            return true;
                        }
                    }
                }
                return false;
                }));
        }

        void cc(uint32_t offset, arm64_cc cc) {
            founds.emplace_back(offset, found::find_any([cc](const cs_insn& insn) {
                if (insn.detail->arm64.cc == cc) {
                    return true;
                }
                return false;
                }));
        }

        template<typename... Args>
        void is(uint32_t offset, Args &&... args) {
            founds.emplace_back(offset, found::find_mnemonic(std::forward<Args>(args)...));
        }

        template<typename... Args>
        void is_reg(uint32_t offset, int reg_index, Args &&... args) {
            founds.emplace_back(offset, found::find_reg(reg_index, std::forward<Args>(args)...));
        }

        template<typename... Args>
        void has_reg(uint32_t offset, Args &&... args) {
            founds.emplace_back(offset, found::find_reg(std::forward<Args>(args)...));
        }

        template<typename... Args>
        void is_mem_base(uint32_t offset, int op_index, Args &&... args) {
            founds.emplace_back(offset, found::find_mem_base(op_index, std::forward<Args>(args)...));
        }

        template<typename... Args>
        void mem_disp(uint32_t offset, int op_index, Args &&... args) {
            founds.emplace_back(offset, found::find_mem_disp(op_index, std::forward<Args>(args)...));
        }

        template<typename... Args>
        void imm(uint32_t offset, int op_index, Args &&... args) {
            founds.emplace_back(offset, found::find_imm(op_index, std::forward<Args>(args)...));
        }

        void st(uint32_t offset, arm64_reg base, int disp) {
            stack stack = { base, disp };
            st(offset, std::move(stack));
        }

        void ld(uint32_t offset, arm64_reg base, int disp) {
            stack stack = { base, disp };
            ld(offset, std::move(stack));
        }

        template<typename... Args>
        void st(uint32_t offset, Args &&... args) {
            founds.emplace_back(offset, found::find_st(std::forward<Args>(args)...));
        }

        template<typename... Args>
        void ld(uint32_t offset, Args &&... args) {
            founds.emplace_back(offset, found::find_ld(std::forward<Args>(args)...));
        }

        void is_ret(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_RET));
        }

        void is_bl(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_BL));
        }

        void is_ldp(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_LDP));
        }

        void is_ldr(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_LDR));
        }

        void is_mov(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_MOV));
        }

        void is_movk(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_MOVK));
        }

        void is_str(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_STR));
        }

        void is_stp(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_STP));
        }

        void is_add(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_ADD));
        }

        void is_b(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_B));
        }

        void is_blr(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_BLR));
        }

        void is_eor(uint32_t offset = 0) {
            founds.emplace_back(offset, found::find_mnemonic(ARM64_INS_EOR));
        }

        void op(uint32_t offset, std::string op) {
            founds.emplace_back(offset, found::find_op(std::move(op)));
        }

        void break_when_b(uint32_t offset = 0) {
            break_.emplace_back(offset, found::find_mnemonic(ARM64_INS_B));
        }

        void break_when(uint32_t offset, std::function<bool(const cs_insn& insn)> any) {
            break_.emplace_back(offset, found::find_any(std::move(any)));
        }

        void break_when(uint32_t offset, arm64_insn op) {
            break_.emplace_back(offset, found::find_mnemonic(op));
        }

        template<typename... Args>
        void break_when_reg_is(uint32_t offset, int reg_index, Args &&... args) {
            break_.emplace_back(offset, found::find_reg(reg_index, std::forward<Args>(args)...));
        }

        template<typename... Args>
        void break_when_has_reg(uint32_t offset, Args &&... args) {
            break_.emplace_back(offset, found::find_reg(std::forward<Args>(args)...));
        }
    };

    class base {
        uint32_t start_index;
        uint32_t end_index;
    protected:
        cs_insn* insn = nullptr;
    public:
        base(uint32_t start_index_,
            uint32_t end_index_,
            cs_insn* insn_) {
            start_index = start_index_;
            end_index = end_index_;
            insn = insn_;
        }

        uint64_t address(uint32_t index) {
            return insn[index].address;
        }

        cs_insn* get_insn(uint32_t index) {
            return &insn[index];
        }

        uint64_t start_address() {
            return insn[start_index].address;
        }

        uint64_t end_address() {
            return insn[end_index].address;
        }

        uint32_t start_i() {
            return start_index;
        }

        uint32_t end_i() {
            return end_index;
        }

        uint32_t find(const std::function<void(finder&)>& finder_, const std::function<void(finder&)>& end_, uint32_t start, uint32_t range) {
            finder end;
            end_(end);
            end.limit(end_index);
            finder f;
            finder_(f);
            f.limit(end_index);

            uint32_t end_i = end.find_range(start + 1, range, insn);
            if (!end_i) return 0;

            return f.find_range(start + 1, end_i - start, insn);
        }

        uint32_t find(const std::function<void(finder&)>& finder_, uint32_t start, uint32_t range) {
            finder f;
            finder_(f);
            f.limit(end_index);
            return f.find_range(start + 1, range, insn);
        }

        uint32_t find(finder f, uint32_t start, uint32_t range) {
            f.limit(end_index);
            return f.find_range(start + 1, range, insn);
        }

        uint32_t find(finder f) {
            f.limit(end_index);
            return f.find_range(start_index, end_index - start_index, insn);
        }

        uint32_t find(const std::function<void(finder&)>& finder_) {
            finder f;
            finder_(f);
            f.limit(end_index);
            return f.find_range(start_index, end_index - start_index, insn);
        }

        uint32_t find_reverse(const std::function<void(finder&)>& finder_, uint32_t start, uint32_t range) {
            if (start == 0) return 0;
            finder f;
            finder_(f);
            f.limit(end_index);
            return f.find_range_reverse(start - 1, range, insn);
        }

        uint32_t find_all(const std::function<void(finder&)>& finder_, std::vector<uint32_t>& indecies) {
            finder f;
            finder_(f);
            f.limit(end_index);
            uint32_t f_index = start_index;
            while (true) {
                f_index = f.find_range(f_index, end_index - f_index, insn);
                if (f_index == 0) {
                    break;
                }
                indecies.push_back(f_index);
                f_index += 1;
            }

            return indecies.size();
        }

        uint32_t find_all(finder f, std::vector<uint32_t>& indecies) {
            f.limit(end_index);
            uint32_t f_index = start_index;
            while (true) {
                f_index = f.find_range(f_index, end_index - f_index, insn);
                if (f_index == 0) {
                    break;
                }
                indecies.push_back(f_index);
                f_index += 1;
            }

            return indecies.size();
        }

        uint32_t find_all(uint32_t start_i, uint32_t end_i, const std::function<void(finder&)>& finder_, std::vector<uint32_t>& indecies) {
            finder f;
            finder_(f);
            f.limit(std::min(end_i, end_index));
            uint32_t f_index = start_i;
            while (true) {
                f_index = f.find_range(f_index, end_i - f_index, insn);
                if (f_index == 0) {
                    break;
                }
                indecies.push_back(f_index);
                f_index += 1;
            }

            return indecies.size();
        }

        bool is(uint32_t index, arm64_insn id) {
            return found(id).find(insn[index]);
        }

        bool reg_is(uint32_t index, arm64_reg id) {
            return found(id).find(insn[index]);
        }

        arm64_insn id(uint32_t index) {
            return (arm64_insn)get_insn(index)->id;
        }

        arm64_reg reg(uint32_t index, uint8_t opt_index) {
            if (insn[index].detail->arm64.op_count <= opt_index) {
                return ARM64_REG_INVALID;
            }
            return insn[index].detail->arm64.operands[opt_index].reg;
        }

        bool type_is(uint32_t index, uint8_t opt_index, arm64_op_type type) {
            if (insn[index].detail->arm64.op_count <= opt_index) {
                return false;
            }
            return insn[index].detail->arm64.operands[opt_index].type == type;
        }

        int64_t imm(uint32_t index, uint8_t opt_index) {
            if (insn[index].detail->arm64.op_count <= opt_index ||
                insn[index].detail->arm64.operands[opt_index].type != ARM64_OP_IMM) {
                return 0;
            }
            return insn[index].detail->arm64.operands[opt_index].imm;
        }

        int32_t mem_disp(uint32_t index, uint8_t opt_index) {
            if (insn[index].detail->arm64.op_count <= opt_index ||
                insn[index].detail->arm64.operands[opt_index].type != ARM64_OP_MEM) {
                return 0;
            }
            return insn[index].detail->arm64.operands[opt_index].mem.disp;
        }

        int32_t mem_disp(uint32_t index, uint8_t opt_index, arm64_reg reg) {
            int reg_i = reg_index(index, reg);
            if (reg_i == -1) return 0;
            int32_t disp = mem_disp(index, opt_index);
            if (disp == 0) return 0;
            return disp + (reg_i == 0 ? 0 : 8);
        }

        int32_t ld_mem_disp(uint32_t index, arm64_reg reg) {
            int reg_i = reg_index(index, reg);
            if (reg_i == -1) return 0;
            int32_t disp = mem_disp(index, insn[index].id == ARM64_INS_LDR ? 1 : 2);
            if (disp == 0) return 0;
            return disp + (reg_i == 0 ? 0 : 8);
        }

        arm64_reg reg(uint32_t index, uint8_t opt_index, int32_t disp) {
            int32_t d = mem_disp(index, opt_index);
            if (d == 0) return ARM64_REG_INVALID;
            int off = disp - d;
            if (off == 0) return reg(index, 0);
            else if (off == 8) return reg(index, 1);
            else return ARM64_REG_INVALID;
        }

        arm64_reg ld_reg(uint32_t index, int32_t disp) {
            arm64_insn insn = id(index);

            int opt_index;
            if (insn == ARM64_INS_LDR)opt_index = 1;
            else if (insn == ARM64_INS_LDP)opt_index = 2;
            else return ARM64_REG_INVALID;

            return reg(index, opt_index, disp);
        }

        arm64_reg st_reg(uint32_t index, int32_t disp) {
            arm64_insn insn = id(index);

            int opt_index;
            if (insn == ARM64_INS_STR)opt_index = 1;
            else if (insn == ARM64_INS_STP)opt_index = 2;
            else return ARM64_REG_INVALID;

            return reg(index, opt_index, disp);
        }

        arm64_reg mem_base(uint32_t index, uint8_t opt_index) {
            if (insn[index].detail->arm64.op_count <= opt_index ||
                insn[index].detail->arm64.operands[opt_index].type != ARM64_OP_MEM) {
                return ARM64_REG_INVALID;
            }
            return insn[index].detail->arm64.operands[opt_index].mem.base;
        }

        int reg_index(uint32_t index, arm64_reg id) {
            for (uint8_t i = 0; i < insn[index].detail->arm64.op_count; ++i) {
                if (insn[index].detail->arm64.operands[i].type == ARM64_OP_REG &&
                    insn[index].detail->arm64.operands[i].reg == id) {
                    return i;
                }
            }
            return -1;
        }

        uint64_t mem_disp(uint32_t index, arm64_reg id) {
            for (uint8_t i = 0; i < insn[index].detail->arm64.op_count; ++i) {
                if (insn[index].detail->arm64.operands[i].type == ARM64_OP_MEM &&
                    insn[index].detail->arm64.operands[i].mem.base == id) {
                    return insn[index].detail->arm64.operands[i].mem.disp;
                }
            }
            return -1;
        }

        int ld_sp(uint32_t index, arm64_reg reg, char* op_str) {
            if (is(index, ARM64_INS_LDR)) {
                strcpy(op_str, strchr(get_insn(index)->op_str, '['));
            }
            else {
                int r_i = reg_index(index, reg);
                if (r_i == 0) {
                    strcpy(op_str, strchr(get_insn(index)->op_str, '['));
                }
                else if (r_i == 1) {
                    uint64_t i = mem_disp(index, ARM64_REG_SP);
                    if (i < 0) {
                        return -1;
                    }
                    sprintf(op_str, "[sp, #0x%lx]", i + 8);
                }
                else {
                    return -1;
                }
            }
            return 0;
        }

    };

    struct point {
        uint64_t address;
        arm64_reg reg = ARM64_REG_INVALID;
    };

    class method : public base {
        std::unordered_map<std::string, point> points;
    public:
        method(uint32_t start, uint32_t end, cs_insn* insn) : base(start, end, insn) {
        }

        point* get_point(const char* name) {
            auto it = points.find(name);
            if (it == points.end()) {
                return nullptr;
            }
            return &it->second;
        }

        void add_point(const char* name, uint64_t address, arm64_reg reg) {
            points[name] = { address, reg };
        }

        void add_point(const char* name, uint32_t index, arm64_reg reg) {
            points[name] = { insn[index].address, reg };
        }

        void add_point(const char* name, uint64_t address) {
            points[name] = { address };
        }

        void add_point(const char* name, uint32_t index) {
            points[name] = { insn[index].address };
        }

    };

    class shellcode {
        struct patch_record {
            uint64_t address = 0;
            std::vector<uint8_t> bytes;
        };

        uint32_t count = 0;
        std::unordered_map<std::string, method> methods;
        std::vector<uint64_t> method_requests_;
        std::vector<patch_record> patches_;
        std::unique_ptr<uint8_t[]> data_;
        uint64_t start_address = 0;
        uint64_t end_address = 0;
        uint64_t size_ = 0;
        cs_insn* insn = nullptr;
    public:

        shellcode() {
        }

        ~shellcode() {
            if (insn) cs_free(insn, count);
        }

        uint64_t size() {
            return size_;
        }

        void* data() {
            return data_.get();
        }

        uint64_t start_addr() {
            return start_address;
        }

        uint64_t end_addr() {
            return end_address;
        }

        void release_analysis_storage() noexcept {
            if (insn) {
                cs_free(insn, count);
                insn = nullptr;
            }
            std::unordered_map<std::string, method>().swap(methods);
            std::vector<uint64_t>().swap(method_requests_);
            data_.reset();
            count = 0;
        }

        void reset() {
            release_analysis_storage();
            patches_.clear();
            start_address = 0;
            end_address = 0;
            size_ = 0;
        }

        void patch(uint32_t i, void* data, uint32_t len) {
            if (insn == nullptr || i >= count) return;
            uint64_t addr = address(i);
            if (addr == 0 || data == nullptr || len == 0 ||
                addr < start_address ||
                addr - start_address > size_ ||
                static_cast<uint64_t>(len) > size_ - (addr - start_address)) {
                return;
            }

            patch_record record;
            record.address = addr;
            record.bytes.resize(len);
            memcpy(record.bytes.data(), data, len);
            patches_.push_back(std::move(record));
            memcpy(data_.get() + addr - start_address, data, len);
        }

        bool apply_patches(uint64_t page_address, void* data, std::size_t size) const {
            if (data == nullptr || size == 0 ||
                page_address > std::numeric_limits<uint64_t>::max() - size) {
                return false;
            }

            const uint64_t page_end = page_address + size;
            bool applied = false;
            auto* destination = static_cast<uint8_t*>(data);
            for (const patch_record& record : patches_) {
                if (record.bytes.empty() ||
                    record.address > std::numeric_limits<uint64_t>::max() -
                        record.bytes.size()) {
                    continue;
                }

                const uint64_t patch_end =
                    record.address + record.bytes.size();
                if (record.address >= page_end || patch_end <= page_address) {
                    continue;
                }

                const uint64_t copy_start =
                    record.address > page_address ? record.address : page_address;
                const uint64_t copy_end = patch_end < page_end ? patch_end : page_end;
                const std::size_t copy_size =
                    static_cast<std::size_t>(copy_end - copy_start);
                memcpy(
                    destination + (copy_start - page_address),
                    record.bytes.data() + (copy_start - record.address),
                    copy_size);
                applied = true;
            }
            return applied;
        }

        const std::vector<uint64_t>& requested_method_addresses() const noexcept {
            return method_requests_;
        }

        int parse(uint64_t base, void* data, uint32_t size) {
            reset();

            start_address = base;

            data_ = std::make_unique<uint8_t[]>(size);
            memcpy(data_.get(), data, size);

            csh handle;
            if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
                return -1;
            }

            cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);
            cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

            size_ = size;
            end_address = start_address + size;
            count = static_cast<uint32_t>(cs_disasm(
                handle, data_.get(), size, start_address, 0, &insn));
            cs_close(&handle);
            return count != 0 && insn != nullptr ? 0 : -1;
        }

        cs_insn* get_insn(uint32_t index) {
            return &insn[index];
        }

        uint64_t address(uint32_t index) {
            return insn[index].address;
        }

        uint32_t index(uint64_t address) {
            if (insn == nullptr || address < start_address ||
                address >= end_address) {
                return count;
            }

            uint32_t i = (address - start_address) / 4;
            if (i < count && insn[i].address == address) {
                return i;
            }

            return count;
        }

        method* get_method(const char* name) {
            auto it = methods.find(name);
            if (it == methods.end()) {
                return nullptr;
            }
            return &it->second;
        }

        method* create_method(const char* name, uint64_t address, finder& end, uint32_t end_range) {
            method_requests_.push_back(address);
            uint32_t start_i = index(address);
            if (start_i >= count) return nullptr;
            end.range(end_range);
            end.limit(count);
            uint32_t end_i = end.find_range(start_i, insn);
            if (!end_i) return nullptr;
            return &methods.insert({ name, method(start_i, end_i, insn) }).first->second;
        }

        method* create_method(const char* name, uint64_t address, const std::function<void(finder&)>& end, uint32_t end_range) {
            method_requests_.push_back(address);
            uint32_t start_i = index(address);
            if (start_i >= count) return nullptr;
            finder f;
            end(f);
            f.range(end_range);
            f.limit(count);
            uint32_t end_i = f.find_range(start_i, insn);
            if (!end_i) return nullptr;
            return &methods.insert({ name, method(start_i, end_i, insn) }).first->second;
        }

    };

    class Find {
    protected:
        shellcode binary_;
    public:
        method* create_method(const char* name, uint64_t address, const std::function<void(finder&)>& end, uint32_t end_range) {
            return binary_.create_method(name, address, end, end_range);
        }

        int set(uint64_t base, void* data, uint32_t size) {
            return binary_.parse(base, data, size);
        };

        shellcode* get_shellcode() {
            return &binary_;
        }
        static bool isW(arm64_reg reg) {
            return reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30;
        }

        static arm64_reg toW(arm64_reg reg) {
            return isW(reg) ? reg : (arm64_reg)(reg + ARM64_REG_W0 - ARM64_REG_X0);
        }
    };

}  // namespace lengjing::game::native::coordinate_pool_internal
