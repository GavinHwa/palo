// Modifications copyright (C) 2017, Baidu.com, Inc.
// Copyright 2017 The Apache Software Foundation

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exprs/expr.h"
#include "udf/udf.h"

#ifdef IR_COMPILE

// Compile ExprContext declaration to IR so we can use it in codegen'd functions
#include "exprs/expr_context.h"

// Dummy function to force compilation of UDF types.
// The arguments are pointers to prevent Clang from lowering the struct types
// (e.g. IntVal={bool, i32} can be coerced to i64).
void dummy(palo_udf::FunctionContext*, palo_udf::BooleanVal*, palo_udf::TinyIntVal*,
    palo_udf::SmallIntVal*, palo_udf::IntVal*, palo_udf::BigIntVal*,
    palo_udf::FloatVal*, palo_udf::DoubleVal*, palo_udf::StringVal*,
    palo_udf::DateTimeVal*, palo_udf::DecimalVal*, palo::ExprContext*) { }
#endif

// The following are compute functions that are cross-compiled to both native and IR
// libraries. In the interpreted path, these functions are executed as-is from the native
// code. In the codegen'd path, we load the IR functions and replace the Get*Val() calls
// with the appropriate child's codegen'd compute function.

namespace palo {
// Static wrappers around Get*Val() functions. We'd like to be able to call these from
// directly from native code as well as from generated IR functions.

BooleanVal Expr::get_boolean_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_boolean_val(context, row);
}
TinyIntVal Expr::get_tiny_int_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_tiny_int_val(context, row);
}
SmallIntVal Expr::get_small_int_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_small_int_val(context, row);
}
IntVal Expr::get_int_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_int_val(context, row);
}
BigIntVal Expr::get_big_int_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_big_int_val(context, row);
}
LargeIntVal Expr::get_large_int_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_large_int_val(context, row);
}
FloatVal Expr::get_float_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_float_val(context, row);
}
DoubleVal Expr::get_double_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_double_val(context, row);
}
StringVal Expr::get_string_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_string_val(context, row);
}
DateTimeVal Expr::get_datetime_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_datetime_val(context, row);
}
DecimalVal Expr::get_decimal_val(Expr* expr, ExprContext* context, TupleRow* row) {
    return expr->get_decimal_val(context, row);
}
}
