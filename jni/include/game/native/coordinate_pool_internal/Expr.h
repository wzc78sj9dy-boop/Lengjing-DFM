#pragma once

#include <utility>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <istream>
#include <ostream>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include "DecFormat.h"
#include "capstone/capstone.h"

namespace lengjing::game::native::coordinate_pool_internal {
enum ExprType {
	EXPR_CONST = 1,
	EXPR_VAR = 2,
	EXPR_BINARY = 3,
	EXPR_SHIFT = 4,
	EXPR_UNARY = 5,
	EXPR_MEMORY = 6
};

struct mem_param {
	std::string name;
	int32_t disp;
	uint32_t size;
	std::vector<int32_t> offset;
	bool runtime = false;
	bool operator<(const mem_param& other) const {
		return std::tie(runtime, disp, offset, name) <
			std::tie(other.runtime, other.disp, other.offset, other.name);
	}
};

class ConstExpr;

class Expr {
public:
	Expr(uint64_t addr) {
		addr_ = addr;
	}
	uint64_t addr_;
	std::string str_name;
	virtual ExprType kind() const noexcept = 0;

	virtual uint64_t eval(
		const std::unordered_map<std::string, uint64_t>& vars
	) = 0;

	virtual void serialize(std::ostream& os) = 0;

	virtual std::string str(std::vector<std::string>& expr_str) = 0;

	virtual void param(std::set<mem_param>& p) {};
	virtual void dependencies(std::set<std::string>& names) const {}
	virtual void list(std::unordered_map<std::shared_ptr<Expr>, uint32_t>& list) {};

	virtual ~Expr() {}
};

class ConstExpr : public Expr {
public:
	uint64_t value;

	ConstExpr(uint64_t v, uint64_t addr)
		: value(v), Expr(addr) {
	}
	ExprType kind() const noexcept override { return EXPR_CONST; }

	uint64_t eval(
		const std::unordered_map<std::string, uint64_t>&
	) override {
		return value;
	}

	std::string str(std::vector<std::string>&) override {
		char buf[64];
		sprintf(buf, "0x%llX", static_cast<unsigned long long>(value));
		return buf;
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_CONST;
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&value), sizeof(value));
	}
};

class VarExpr : public Expr {
public:
	std::string name;

	VarExpr(std::string n, uint64_t addr)
		: name(std::move(n)), Expr(addr) {
	}
	ExprType kind() const noexcept override { return EXPR_VAR; }

	uint64_t eval(
		const std::unordered_map<std::string, uint64_t>& vars
	) override {
		return vars.at(name);
	}

	std::string str(std::vector<std::string>&) override {
		return name;
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_VAR;
		size_t len = name.size();
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&len), sizeof(len));
		os.write(name.data(), len);
	}

	void dependencies(std::set<std::string>& names) const override {
		names.insert(name);
	}
};

class MemVarExpr : public Expr {
public:
	std::string base_;
	uint16_t id = 0;
	std::string name;
	uint32_t size_;
	int32_t disp_;
	int32_t offset = 0;
	std::vector<int32_t> off_;
	bool runtime_ = false;

	MemVarExpr(std::string n, uint32_t size, int32_t disp, uint64_t addr, std::string base)
		: name(std::move(n)), size_(size), disp_(disp), Expr(addr), base_(std::move(base)) {
	}
	ExprType kind() const noexcept override { return EXPR_MEMORY; }

	std::shared_ptr<MemVarExpr> to(uint32_t size, int32_t off) {
		auto expr = std::make_shared<MemVarExpr>(*this);
		expr->off_.push_back(off + offset);
		expr->name = coordinate_pool_format::Format("({}+0x{:X}_{})", base_, disp_, id);
		expr->size_ = size;
		expr->offset = 0;
		expr->id = id++;
		return expr;
	}

	std::shared_ptr<MemVarExpr> to_runtime(uint32_t size, int32_t off) {
		auto expr = std::make_shared<MemVarExpr>(*this);
		expr->off_.push_back(off);
		expr->size_ = size;
		expr->offset = 0;
		expr->runtime_ = true;
		return expr;
	}

	uint64_t eval(
		const std::unordered_map<std::string, uint64_t>& vars
	) override {
		return vars.at(name);
	}

	std::string str(std::vector<std::string>&) override {
		std::string out = name;
		for (uint32_t d : off_) {
			if (d == 0) {
				out = coordinate_pool_format::Format("*{}", out, d);
			}
			else {
				out = coordinate_pool_format::Format("*({} {} {})", out, d < 0 ? '-' : '+', d);
			}
		}
		return out;
	}

	void param(std::set<mem_param>& p) override {
		p.insert({ name, disp_, size_, off_, runtime_ });
	}

	void dependencies(std::set<std::string>& names) const override {
		names.insert(name);
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_VAR;
		size_t len = name.size();
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&len), sizeof(len));
		os.write(name.data(), len);
	}
};

enum UnaryOp {
	OP_MVN,
	OP_NEG,
};

class UnaryExpr : public Expr {
public:
	UnaryOp op;
	std::shared_ptr<Expr> operand;

	UnaryExpr(UnaryOp o, std::shared_ptr<Expr> e, uint64_t addr) : op(o), operand(e), Expr(addr) {}
	ExprType kind() const noexcept override { return EXPR_UNARY; }

	uint64_t eval(const std::unordered_map<std::string, uint64_t>& vars) override {
		uint64_t val = operand->eval(vars);
		switch (op) {
		case OP_MVN: return ~val;
		case OP_NEG: return (-val);
		}
		return 0;
	}
	std::string str(std::vector<std::string>& expr_str) override {
		std::string o;
		switch (op) {
		case OP_MVN: o = "~"; break;
		case OP_NEG: o = "-"; break;
		}

		if (!str_name.empty()) {
			expr_str.push_back(coordinate_pool_format::Format("{} = {}{}", str_name, o, operand->str(expr_str)));
			return str_name;
		}

		return coordinate_pool_format::Format("({}{})", o, operand->str(expr_str));
	}

	void param(std::set<mem_param>& p) override {
		operand->param(p);
	}

	void dependencies(std::set<std::string>& names) const override {
		operand->dependencies(names);
	}

	void list(std::unordered_map<std::shared_ptr<Expr>, uint32_t>& list) override {
		operand->list(list);
		list[operand]++;
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_UNARY;
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&op), sizeof(op));
		operand->serialize(os);
	}
};

enum BinOp {
	OP_ADD,
	OP_MUL,
	OP_SUB,
	OP_XOR,
	OP_AND,
	OP_OR,
	OP_ORN,
	OP_LSR,
	OP_LSL,
	OP_BIC,
	OP_UMULH
};

class BinaryExpr : public Expr {
public:

	BinOp op;

	std::shared_ptr<Expr> lhs;
	std::shared_ptr<Expr> rhs;

	BinaryExpr(
		BinOp o,
		std::shared_ptr<Expr> a,
		std::shared_ptr<Expr> b,
		uint64_t addr
	) : Expr(addr) {
		op = o;
		lhs = a;
		rhs = b;
	}
	ExprType kind() const noexcept override { return EXPR_BINARY; }

	uint64_t eval(
		const std::unordered_map<std::string, uint64_t>& vars
	) override {

		uint64_t A = lhs->eval(vars);
		uint64_t B = rhs->eval(vars);

		switch (op) {

		case OP_ADD:
			return A + B;
		case OP_MUL:
			return A * B;
		case OP_SUB:
			return A - B;

		case OP_XOR:
			return A ^ B;

		case OP_AND:
			return A & B;

		case OP_OR:
			return A | B;
		case OP_ORN:
			return A | ~B;
		case OP_LSR:
			return A >> B;

		case OP_LSL:
			return A << B;

		case OP_BIC:
			return A & (~B);

		case OP_UMULH:
#if defined(_MSC_VER)
			uint64_t High;
			_umul128(A, B, &High);
			return High;
#else
			return (uint64_t)(((unsigned __int128)A * (unsigned __int128)B) >> 64);
#endif
		}

		return 0;
	}

	std::string str(std::vector<std::string>& expr_str) override {

		std::string o;

		switch (op) {

		case OP_ADD:
			o = "+";
			break;
		case OP_MUL:
		case OP_UMULH:
			o = "*";
			break;
		case OP_SUB:
			o = "-";
			break;
		case OP_XOR:
			o = "^";
			break;
		case OP_AND:
			o = "&";
			break;
		case OP_OR:
			o = "|";
			break;
		case OP_ORN:
			o = "|~";
			break;
		case OP_LSR:
			o = ">>";
			break;
		case OP_LSL:
			o = "<<";
			break;
		case OP_BIC:
			o = "&~";
			break;
		}

		if (!str_name.empty()) {
			expr_str.push_back(coordinate_pool_format::Format("{} = {} {} {}", str_name, lhs->str(expr_str), o, rhs->str(expr_str)));
			return str_name;
		}

		return coordinate_pool_format::Format("({} {} {})", lhs->str(expr_str), o, rhs->str(expr_str));
	}

	void param(std::set<mem_param>& p) override {
		lhs->param(p);
		rhs->param(p);
	}

	void dependencies(std::set<std::string>& names) const override {
		lhs->dependencies(names);
		rhs->dependencies(names);
	}

	void list(std::unordered_map<std::shared_ptr<Expr>, uint32_t>& list) override {
		lhs->list(list);
		rhs->list(list);
		list[lhs]++;
		list[rhs]++;
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_BINARY;
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&op), sizeof(op));
		lhs->serialize(os);
		rhs->serialize(os);
	}
};

enum ShiftOp {
	SHIFT_OP_LSL,
	SHIFT_OP_LSR,
};

class ShiftExpr : public Expr {
public:

	ShiftOp op;

	std::shared_ptr<Expr> expr;

	int amount;

	ShiftExpr(
		ShiftOp o,
		std::shared_ptr<Expr> e,
		int amt,
		uint64_t addr
	) :Expr(addr) {
		op = o;
		expr = e;
		amount = amt;
	}
	ExprType kind() const noexcept override { return EXPR_SHIFT; }

	uint64_t eval(
		const std::unordered_map<std::string, uint64_t>& vars
	) override {

		uint64_t v = expr->eval(vars);

		if (op == SHIFT_OP_LSL)
			return v << amount;

		return v >> amount;
	}

	std::string str(std::vector<std::string>& expr_str) override {
		return "("
			+ expr->str(expr_str)
			+ (op == SHIFT_OP_LSR ? " >> " : " << ")
			+ std::to_string(amount)
			+ ")";
	}

	void param(std::set<mem_param>& p) override {
		expr->param(p);
	}

	void dependencies(std::set<std::string>& names) const override {
		expr->dependencies(names);
	}

	void list(std::unordered_map<std::shared_ptr<Expr>, uint32_t>& list) override {
		expr->list(list);
		list[expr]++;
	}

	void serialize(std::ostream& os) override {
		int type = EXPR_SHIFT;
		int op_value = static_cast<int>(op);
		os.write(reinterpret_cast<const char*>(&type), sizeof(type));
		os.write(reinterpret_cast<const char*>(&op_value), sizeof(op_value));
		os.write(reinterpret_cast<const char*>(&amount), sizeof(amount));
		expr->serialize(os);
	}
};

}  // namespace lengjing::game::native::coordinate_pool_internal

