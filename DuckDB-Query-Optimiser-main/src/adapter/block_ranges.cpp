// The ranges of blocks (ADR-0005).

#include "block_ranges.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

namespace recycling {

// Types whose constants Value orders as a DuckDB filter compares them.
static bool HasOrderedConstants(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DATE:
		return true;
	default:
		return false;
	}
}

static bool IsColumnOfOrderedType(const Expression &expression) {
	return expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	       HasOrderedConstants(expression.return_type);
}

// Before the optimizers run, a constant may still be a cast of one.
static bool IsConstantOf(ClientContext &context, const Expression &expression, const LogicalType &type, Value &value) {
	if (!expression.IsFoldable() || expression.return_type != type ||
	    !ExpressionExecutor::TryEvaluateScalar(context, expression, value)) {
		return false;
	}
	return !value.IsNull() && value.type() == type;
}

bool ExtractRange(ClientContext &context, const Expression &conjunct, ColumnBinding &column, BlockRange &range) {
	switch (conjunct.GetExpressionClass()) {
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = conjunct.Cast<BoundBetweenExpression>();
		if (!IsColumnOfOrderedType(*between.input) ||
		    !IsConstantOf(context, *between.lower, between.input->return_type, range.lower) ||
		    !IsConstantOf(context, *between.upper, between.input->return_type, range.upper)) {
			return false;
		}
		range.lower_inclusive = between.lower_inclusive;
		range.upper_inclusive = between.upper_inclusive;
		column = between.input->Cast<BoundColumnRefExpression>().binding;
		return true;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comparison = conjunct.Cast<BoundComparisonExpression>();
		bool flipped = !IsColumnOfOrderedType(*comparison.left);
		auto &column_side = flipped ? *comparison.right : *comparison.left;
		Value value;
		if (!IsColumnOfOrderedType(column_side) ||
		    !IsConstantOf(context, flipped ? *comparison.left : *comparison.right, column_side.return_type, value)) {
			return false;
		}
		// Written as `constant < column`, the comparison is `column > constant`.
		auto type = flipped ? FlipComparisonExpression(conjunct.GetExpressionType()) : conjunct.GetExpressionType();
		switch (type) {
		case ExpressionType::COMPARE_EQUAL:
			range.lower = value;
			range.upper = value;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			range.lower = value;
			range.lower_inclusive = type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			range.upper = value;
			range.upper_inclusive = type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
			break;
		default:
			return false;
		}
		column = column_side.Cast<BoundColumnRefExpression>().binding;
		return true;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = conjunct.Cast<BoundOperatorExpression>();
		if (conjunct.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL || op.children.size() != 1 ||
		    !IsColumnOfOrderedType(*op.children[0])) {
			return false;
		}
		column = op.children[0]->Cast<BoundColumnRefExpression>().binding;
		return true;
	}
	default:
		return false;
	}
}

void Intersect(BlockRange &range, const BlockRange &other) {
	if (!other.lower.IsNull() &&
	    (range.lower.IsNull() || other.lower > range.lower || (other.lower == range.lower && !other.lower_inclusive))) {
		range.lower = other.lower;
		range.lower_inclusive = other.lower_inclusive;
	}
	if (!other.upper.IsNull() &&
	    (range.upper.IsNull() || other.upper < range.upper || (other.upper == range.upper && !other.upper_inclusive))) {
		range.upper = other.upper;
		range.upper_inclusive = other.upper_inclusive;
	}
}

bool RangesContain(const BlockRanges &outer, const BlockRanges &inner) {
	for (auto &entry : outer) {
		auto found = inner.find(entry.first);
		if (found == inner.end()) {
			return false;
		}
		auto &out = entry.second;
		auto &in = found->second;
		if (!out.lower.IsNull() && (in.lower.IsNull() || in.lower < out.lower ||
		                            (in.lower == out.lower && in.lower_inclusive && !out.lower_inclusive))) {
			return false;
		}
		if (!out.upper.IsNull() && (in.upper.IsNull() || in.upper > out.upper ||
		                            (in.upper == out.upper && in.upper_inclusive && !out.upper_inclusive))) {
			return false;
		}
	}
	return true;
}

string RangesKey(const BlockRanges &ranges) {
	string result;
	for (auto &entry : ranges) {
		auto &range = entry.second;
		result += "|" + to_string(entry.first.first) + "." + to_string(entry.first.second) + ":";
		result += range.lower.IsNull() ? "*" : (range.lower_inclusive ? "[" : "(") + range.lower.ToString();
		result += ",";
		result += range.upper.IsNull() ? "*" : range.upper.ToString() + (range.upper_inclusive ? "]" : ")");
	}
	return result;
}

} // namespace recycling
} // namespace duckdb
