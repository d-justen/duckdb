#include "core_functions/scalar/list_functions.hpp"

#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

namespace {

static unique_ptr<FunctionData> ListTransformFilterBind(ClientContext &context, ScalarFunction &bound_function,
                                                        vector<unique_ptr<Expression>> &arguments) {
	// the list column and the bound lambda expression
	D_ASSERT(arguments.size() == 2);
	if (arguments[1]->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
		throw BinderException("Invalid lambda expression!");
	}

	auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();

	// try to cast to boolean, if the return type of the lambda filter expression is not already boolean
	if (bound_lambda_expr.lambda_expr->return_type != LogicalType::BOOLEAN) {
		auto cast_lambda_expr =
		    BoundCastExpression::AddCastToType(context, std::move(bound_lambda_expr.lambda_expr), LogicalType::BOOLEAN);
		bound_lambda_expr.lambda_expr = std::move(cast_lambda_expr);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));

	bound_function.SetReturnType(arguments[0]->return_type);
	auto has_index = bound_lambda_expr.parameter_count == 2;
	return LambdaFunctions::ListLambdaBind(context, bound_function, arguments, has_index);
}

static LogicalType ListTransformFilterBindLambda(ClientContext &context, const vector<LogicalType> &function_child_types,
                                                 const idx_t parameter_idx) {
	return LambdaFunctions::BindBinaryChildren(function_child_types, parameter_idx);
}

}

ScalarFunction ListTransformFilterFun::GetFunction() {
	ScalarFunction fun({LogicalType::LIST(LogicalType::ANY), LogicalType::LAMBDA}, LogicalType::LIST(LogicalType::ANY),
					   LambdaFunctions::ListFilterFunction, ListTransformFilterBind, nullptr, nullptr);

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(ListLambdaBindData::Serialize);
	fun.SetDeserializeCallback(ListLambdaBindData::Deserialize);
	fun.SetBindLambdaCallback(ListTransformFilterBindLambda);

	return fun;
}

} // namespace duckdb
