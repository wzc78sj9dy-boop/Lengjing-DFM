#pragma once
#include "Expr.h"
#include <unordered_map>
#include <set>

namespace lengjing::game::native::coordinate_pool_internal {

struct VarParam {
	std::string name;
	uint64_t addr;
	arm64_reg reg;
	uint64_t value;
};

class Analyze {
	uint64_t cur_addr;
	std::unordered_map<arm64_reg, std::shared_ptr<Expr>> regs;
	std::shared_ptr<ConstExpr> make_const(uint64_t v);
	std::shared_ptr<BinaryExpr> make_binary(BinOp o,
		std::shared_ptr<Expr> a,
		std::shared_ptr<Expr> b);
	std::shared_ptr<ShiftExpr> make_shift(ShiftOp o,
		std::shared_ptr<Expr> e,
		int amt);
	std::shared_ptr<UnaryExpr> make_unary(UnaryOp o, std::shared_ptr<Expr> e);
	std::shared_ptr<MemVarExpr> make_mem(std::string n, uint32_t size, int32_t disp, std::string base);
public:
	std::vector<VarParam> varParams;

    void reset()
	{
		varParams.clear();
		regs.clear();
	}
	void setVal(arm64_reg reg, const char* name);
	int parse(cs_insn* insn);
	std::string str(arm64_reg reg);
	void str(arm64_reg reg, std::vector<std::string>& expr);
	uint64_t execute(arm64_reg reg, std::unordered_map<std::string, uint64_t> &params);
	bool list_mem_param(arm64_reg reg, std::set<mem_param>& p) {
		if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30) {
			reg = static_cast<arm64_reg>(
				reg - ARM64_REG_W0 + ARM64_REG_X0);
		}
		auto it = regs.find(reg);
		if (it == regs.end() || !it->second) return false;
		it->second->param(p);
		return true;
	}
	void list_expr(arm64_reg reg, std::unordered_map<std::shared_ptr<Expr>, uint32_t>& list) {
		auto it = regs.find(reg);
		if (it == regs.end())return;
		list[it->second]++;
		it->second->list(list);
	}
	std::shared_ptr<Expr> get_expr(arm64_reg reg);
	void retain_var_params(const std::set<std::string>& dependencies);

};

}  // namespace lengjing::game::native::coordinate_pool_internal
