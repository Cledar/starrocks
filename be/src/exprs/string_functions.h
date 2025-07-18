// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hs/hs.h>
#include <re2/re2.h>
#include <runtime/decimalv3.h>

#include <iomanip>

#include "column/column_builder.h"
#include "column/column_viewer.h"
#include "exprs/function_context.h"
#include "exprs/function_helper.h"
#include "runtime/current_thread.h"
#include "util/phmap/phmap.h"
#include "util/url_parser.h"

namespace starrocks {
class RegexpSplit;

struct PadState {
    bool is_const;
    bool fill_is_const;
    Slice fill;
    bool fill_is_utf8;
    std::vector<size_t> fill_utf8_index;
};

struct SubstrState {
    bool is_const = false;
    int32_t pos = std::numeric_limits<int32_t>::lowest();
    int32_t len = std::numeric_limits<int32_t>::max();
};

struct ConcatState {
    bool is_const = false;
    bool is_oversize = false;
    std::string tail;
};

template <LogicalType T>
struct FieldFuncState {
    bool all_const = false;
    bool list_all_const = false;
    std::map<RunTimeCppType<T>, int> mp;
};

struct StringFunctionsState {
    using DriverMap = phmap::parallel_flat_hash_map<int32_t, std::unique_ptr<re2::RE2>, phmap::Hash<int32_t>,
                                                    phmap::EqualTo<int32_t>, phmap::Allocator<int32_t>,
                                                    NUM_LOCK_SHARD_LOG, std::mutex>;

    std::string pattern;
    std::unique_ptr<re2::RE2> regex;
    std::unique_ptr<re2::RE2::Options> options;
    bool const_pattern{false};
    DriverMap driver_regex_map; // regex for each pipeline_driver, to make it driver-local

    bool use_hyperscan = false;
    int size_of_pattern = -1;

    // a pointer to the generated database that responsible for parsed expression.
    hs_database_t* database = nullptr;
    // a type containing error details that is returned by the compile calls on failure.
    hs_compile_error_t* compile_err = nullptr;
    // A Hyperscan scratch space, Used to call hs_scan,
    // one scratch space per thread, or concurrent caller, is required
    hs_scratch_t* scratch = nullptr;

    StringFunctionsState() : regex(), options() {}

    // Implement a driver-local regex, to avoid lock contention on the RE2::cache_mutex
    re2::RE2* get_or_prepare_regex() {
        DCHECK(const_pattern);
        int32_t driver_id = CurrentThread::current().get_driver_id();
        if (driver_id == 0) {
            return regex.get();
        }
        re2::RE2* res = nullptr;
        driver_regex_map.lazy_emplace_l(
                driver_id, [&](auto& value) { res = value.get(); },
                [&](auto build) {
                    auto regex = std::make_unique<re2::RE2>(pattern, *options);
                    DCHECK(regex->ok());
                    res = regex.get();
                    build(driver_id, std::move(regex));
                });
        DCHECK(!!res);
        return res;
    }

    ~StringFunctionsState() {
        if (scratch != nullptr) {
            hs_free_scratch(scratch);
        }

        if (database != nullptr) {
            hs_free_database(database);
        }
    }
};

struct MatchInfo {
    size_t from;
    size_t to;
};

struct MatchInfoChain {
    std::vector<MatchInfo> info_chain;
};

struct LowerUpperState {
    std::function<StatusOr<ColumnPtr>(const ColumnPtr&)> impl_func;
};

class StringFunctions {
public:
    /**
   * @param: [string_value, position, optional<length>]
   * @paramType: [BinaryColumn, IntColumn, optional<IntColumn>]
   * @return: BinaryColumn
   */
    DEFINE_VECTORIZED_FN(substring);

    /**
     * @param: [string_value, length]
     * @paramType: [BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(left);

    /**
     * @param: [string_value, length]
     * @paramType: [BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(right);

    /**
     * @param: [string_value, prefix]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: BooleanColumn
     */
    DEFINE_VECTORIZED_FN(starts_with);

    /**
     * @param: [string_value, subffix]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: BooleanColumn
     */
    DEFINE_VECTORIZED_FN(ends_with);

    /**
     * Return a string of the specified number of spaces
     *
     * @param: [length]
     * @paramType: [IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(space);

    /**
     * Repeat a string the specified number of times
     * we will truncate the result length to 65535
     *
     * @param: [string_value, times]
     * @paramType: [BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(repeat);

    /**
     * Return the string argument, left-padded with the specified string
     *
     * @param: [string_value, repeat_number]
     * @paramType: [BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(lpad);

    /**
     * Append string the specified number of times
     *
     * @param: [string_value, repeat_number]
     * @paramType: [BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(rpad);

    /**
     * Append the character if the s string is non-empty and does not contain the character at the
     * end, and the character length must be 1.
     *
     * @param: [string_value, tail_char]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(append_trailing_char_if_absent);

    /**
     * Return the length of a string in bytes
     *
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(length);

    /**
     * Return the length of a string in utf8
     *
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(utf8_length);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(lower);
    static Status lower_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status lower_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(upper);
    static Status upper_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status upper_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(reverse);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(trim);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(ltrim);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(rtrim);

    /**
     * Return numeric value of left-most character
     *
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(ascii);

    /**
     * @param: [IntColumn]
     * @return: StringColumn
     * Get symbols from binary numbers
     */
    DEFINE_VECTORIZED_FN(get_char);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BigIntColumn
     */
    DEFINE_VECTORIZED_FN(inet_aton);

    /**
     * Return the index of the first occurrence of substring
     *
     * @param: [string_value, sub_string_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(instr);

    /**
     * Return the position of the first occurrence of substring
     *
     * @param: [sub_string_value, string_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(locate);

    /**
     * Return the position of the first occurrence of substring start with start_position
     *
     * @param: [sub_string_value, string_value, start_position]
     * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(locate_pos);

    /**
     * Return the position of the first occurrence of substring in string
     *
     * @param: [string_value, sub_string_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: BigIntColumn
     */
    DEFINE_VECTORIZED_FN(strpos);

    /**
     * Return the position of the N-th occurrence of substring in string
     * When N is negative, search from the end of string
     *
     * @param: [string_value, sub_string_value, instance]
     * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
     * @return: BigIntColumn
     */
    DEFINE_VECTORIZED_FN(strpos_instance);

    /**
     * @param: [string_value, ......]
     * @paramType: [BinaryColumn, ......]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(concat);

    /**
     * Return concatenate with separator
     *
     * @param: [string_value, ......]
     * @paramType: [BinaryColumn, ......]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(concat_ws);

    /**
     * Index (position) of first argument within second argument which is a comma-separated string
     *
     * @param: [string_value, string_set]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(find_in_set);

    /**
     * @param: [string_value]
     * @paramType: [BinaryColumn]
     * @return: BooleanColumn
     */
    DEFINE_VECTORIZED_FN(null_or_empty);

    /**
     * @param: [string_value, delimiter]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: ArrayColumn
     */
    DEFINE_VECTORIZED_FN(split);

    static Status split_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status split_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
    * @param: [array_string, delimiter]
    * @paramType: [ArrayBinaryColumn, BinaryColumn]
    * @return: MapColumn map<string,string>
    */
    DEFINE_VECTORIZED_FN(str_to_map_v1);

    /**
    * @param: [string, delimiter, map_delimiter]
    * @paramType: [BinaryColumn, BinaryColumn, BinaryColumn]
    * @return: MapColumn map<string,string>
    */
    DEFINE_VECTORIZED_FN(str_to_map);
    static Status str_to_map_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status str_to_map_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [string_value, delimiter, field]
     * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(split_part);

    /**
     * @param: [string_value, delimiter, field]
     * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(substring_index);

    // regex method
    static Status regexp_extract_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status regexp_replace_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status regexp_count_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status regexp_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [string_value, pattern_value, index_value]
     * @paramType: [BinaryColumn, BinaryColumn, Int64Column]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(regexp_extract);

    /**
     * return all match sub-string
     * @param: [string_value, pattern_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: Array<BinaryColumn>
     */
    DEFINE_VECTORIZED_FN(regexp_extract_all);

    /**
     * @param: [string_value, pattern_value, replace_value]
     * @paramType: [BinaryColumn, BinaryColumn, BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(regexp_replace);

    static StatusOr<ColumnPtr> regexp_replace_use_hyperscan(StringFunctionsState* state, const Columns& columns);
    static StatusOr<ColumnPtr> regexp_replace_use_hyperscan_vec(StringFunctionsState* state, const Columns& columns);

    /**
     * @param: [string_value, pattern, max_split]
     * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
     * @return: Array<BinaryColumn>
     */
    DEFINE_VECTORIZED_FN(regexp_split);

    /**
     * @param: [string_value, pattern_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: BigIntColumn
     */
    DEFINE_VECTORIZED_FN(regexp_count);

    /**
     * @param: [string_value, pattern_value, replace_value]
     * @paramType: [BinaryColumn, BinaryColumn, BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(replace);
    static Status replace_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status replace_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [string_value, from_value, to_value]
     * @paramType: [BinaryColumn, BinaryColumn, BinaryColumn]
     * @return: BinaryColumn
     */
    DEFINE_VECTORIZED_FN(translate);
    static Status translate_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status translate_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    /**
     * @param: [DOUBLE]
     * @paramType: [DoubleColumn]
     * @return: BinaryColumn
     */
    static StatusOr<ColumnPtr> money_format_double(FunctionContext* context, const starrocks::Columns& columns);

    /**
     * @param: [BIGINT]
     * @paramType: [Int64Column]
     * @return: BinaryColumn
     */
    static StatusOr<ColumnPtr> money_format_bigint(FunctionContext* context, const starrocks::Columns& columns);

    /**
     * @param: [DECIMALV2]
     * @paramType: [DecimalColumn]
     * @return: BinaryColumn
     */
    static StatusOr<ColumnPtr> money_format_largeint(FunctionContext* context, const starrocks::Columns& columns);

    /**
     * @param: [LARGEINT]
     * @paramType: [Int128Column]
     * @return: BinaryColumn
     */
    static StatusOr<ColumnPtr> money_format_decimalv2val(FunctionContext* context, const starrocks::Columns& columns);

    template <LogicalType Type>
    static StatusOr<ColumnPtr> money_format_decimal(FunctionContext* context, const starrocks::Columns& columns);

    static Status trim_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status trim_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    // parse's auxiliary method
    static Status parse_url_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status parse_url_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status sub_str_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status url_extract_parameter_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status url_extract_parameter_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status sub_str_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status left_or_right_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status left_or_right_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status concat_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status concat_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status pad_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status pad_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    /**
   * string_value is a url, part_value indicate a part of the url, return url's corresponding value;
   * part_values is fixed: "AUTHORITY"/"FILE"/"HOST"/"PROTOCOL" and so on.
   *
   * @param: [string_value, part_value]
   * @paramType: [BinaryColumn, BinaryColumn]
   * @return: BinaryColumn
   */
    DEFINE_VECTORIZED_FN(parse_url);

    DEFINE_VECTORIZED_FN(url_extract_parameter);

    DEFINE_VECTORIZED_FN(url_extract_host);

    /**
     * @param: [BigIntColumn]
     * @return: StringColumn
     * Get the hexadecimal representation of bigint
     */
    DEFINE_VECTORIZED_FN(hex_int);
    /**
     * @param: [StringColumn]
     * @return: StringColumn
     * Get the hexadecimal representation of string
     */
    DEFINE_VECTORIZED_FN(hex_string);
    /**
     * @param: [BigIntColumn]
     * @return: StringColumn
     * Get the string of this hexadecimal representation represents
     */
    DEFINE_VECTORIZED_FN(unhex);
    /**
     * @param: [StringColumn]
     * @return: StringColumn
     * Get the hexadecimal representation of SM3 hash value
     *
     */
    DEFINE_VECTORIZED_FN(sm3);

    /**
     * Compare two strings. Returns 0 if lhs and rhs compare equal,
     * -1 if lhs appears before rhs in lexicographical order,
     * 1 if lhs appears after rhs in lexicographical order.
     *
     * @param: [string_value, string_value]
     * @paramType: [BinaryColumn, BinaryColumn]
     * @return: IntColumn
     */
    DEFINE_VECTORIZED_FN(strcmp);

    /**
     * params are one strings. Returns string for url encode string,
     *
     * @param: [string_value]
     * @paramType: [StringColumn]
     * @return: StringColumn
     */
    DEFINE_VECTORIZED_FN(url_encode);
    static std::string url_encode_func(const std::string& value);

    /**
     * params are one strings. Returns string for url decode string,
     *
     * @param: [string_value]
     * @paramType: [StringColumn]
     * @return: StringColumn
     */
    DEFINE_VECTORIZED_FN(url_decode);
    static std::string url_decode_func(const std::string& value);

    static inline char _DUMMY_STRING_FOR_EMPTY_PATTERN = 'A';

    DEFINE_VECTORIZED_FN(crc32);

    DEFINE_VECTORIZED_FN(ngram_search);

    DEFINE_VECTORIZED_FN(ngram_search_case_insensitive);

    DEFINE_VECTORIZED_FN_TEMPLATE(field);
    template <LogicalType Type>
    static Status field_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    template <LogicalType Type>
    static Status field_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

    static Status ngram_search_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope);
    static Status ngram_search_case_insensitive_prepare(FunctionContext* context,
                                                        FunctionContext::FunctionStateScope scope);
    static Status ngram_search_close(FunctionContext* context, FunctionContext::FunctionStateScope scope);

private:
    static int index_of(const char* source, int source_count, const char* target, int target_count, int from_index);

    static Status hs_compile_and_alloc_scratch(const std::string&, StringFunctionsState*, FunctionContext*,
                                               const Slice& slice);

private:
    struct CurrencyFormat : std::moneypunct<char> {
        pattern do_pos_format() const override { return {{none, sign, none, value}}; }
        pattern do_neg_format() const override { return {{none, sign, none, value}}; }
        int do_frac_digits() const override { return 2; }
        char_type do_thousands_sep() const override { return ','; }
        string_type do_grouping() const override { return "\003"; }
        string_type do_negative_sign() const override { return "-"; }
    };

    static std::string transform_currency_format(FunctionContext* context, const std::string& v) {
        std::locale comma_locale(std::locale(), new CurrencyFormat());
        std::stringstream ss;
        ss.imbue(comma_locale);
        ss << std::put_money(v);
        return ss.str();
    };

    struct ParseUrlState {
        bool const_pattern{false};
        std::unique_ptr<UrlParser::UrlPart> url_part;
        ParseUrlState() : url_part() {}
    };

    struct UrlExtractParameterState {
        std::optional<std::string> opt_const_result;
        bool result_is_null{false};
        std::optional<std::string> opt_const_param_key;
        std::optional<std::string> opt_const_query_params;
        UrlExtractParameterState() = default;
    };

    static StatusOr<ColumnPtr> parse_url_general(FunctionContext* context, const starrocks::Columns& columns);
    static StatusOr<ColumnPtr> parse_const_urlpart(UrlParser::UrlPart* url_part, FunctionContext* context,
                                                   const starrocks::Columns& columns);

    template <LogicalType Type, bool scale_up, bool check_overflow>
    static inline void money_format_decimal_impl(FunctionContext* context, ColumnViewer<Type> const& money_viewer,
                                                 size_t num_rows, int adjust_scale,
                                                 ColumnBuilder<TYPE_VARCHAR>* result);
};

template <bool to_upper>
struct StringCaseToggleFunction {
public:
    template <LogicalType Type, LogicalType ResultType>
    static ColumnPtr evaluate(const ColumnPtr& v1);
};

template <LogicalType Type, bool scale_up, bool check_overflow>
void StringFunctions::money_format_decimal_impl(FunctionContext* context, ColumnViewer<Type> const& money_viewer,
                                                size_t num_rows, int adjust_scale,
                                                ColumnBuilder<TYPE_VARCHAR>* result) {
    using CppType = RunTimeCppType<Type>;
    const auto scale_factor = get_scale_factor<CppType>(adjust_scale);
    static constexpr auto max_precision = decimal_precision_limit<CppType>;
    for (int row = 0; row < num_rows; ++row) {
        if (money_viewer.is_null(row)) {
            result->append_null();
            continue;
        }

        auto money_value = money_viewer.value(row);
        CppType rounded_cent_money;
        auto overflow = DecimalV3Cast::round<CppType, ROUND_HALF_EVEN, scale_up, check_overflow>(
                money_value, scale_factor, &rounded_cent_money);
        std::string concurr_format;
        if (rounded_cent_money == 0) {
            concurr_format = "0.00";
        } else {
            bool is_negative = rounded_cent_money < 0;
            CppType abs_rounded_cent_money = is_negative ? -rounded_cent_money : rounded_cent_money;
            auto str = DecimalV3Cast::to_string<CppType>(abs_rounded_cent_money, max_precision, 0);
            std::string prefix = is_negative ? "-" : "";
            // if there is only fractional part, we need to add leading zeros so that transform_currency_format can work
            if (abs_rounded_cent_money < 100) {
                prefix.append(abs_rounded_cent_money < 10 ? "00" : "0");
            }
            concurr_format = transform_currency_format(context, prefix + str);
        }
        result->append(Slice(concurr_format.data(), concurr_format.size()), overflow);
    }
}

template <LogicalType Type>
StatusOr<ColumnPtr> StringFunctions::money_format_decimal(FunctionContext* context, const starrocks::Columns& columns) {
    RETURN_IF_COLUMNS_ONLY_NULL(columns);
    using CppType = RunTimeCppType<Type>;
    static_assert(lt_is_decimal<Type>, "Invalid decimal type");
    auto money_viewer = ColumnViewer<Type>(columns[0]);
    const auto& type = context->get_arg_type(0);
    int scale = type->scale;

    auto num_rows = columns[0]->size();
    ColumnBuilder<TYPE_VARCHAR> result(num_rows);
    if (scale > 2) {
        // scale down
        money_format_decimal_impl<Type, false, true>(context, money_viewer, num_rows, scale - 2, &result);
    } else {
        // scale up
        money_format_decimal_impl<Type, true, true>(context, money_viewer, num_rows, 2 - scale, &result);
    }
    return result.build(ColumnHelper::is_all_const(columns));
}

template <LogicalType Type>
Status StringFunctions::field_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return Status::OK();
    }

    auto* state = new FieldFuncState<Type>();
    context->set_function_state(scope, state);
    state->list_all_const = true;
    for (int i = 1; i < context->get_num_constant_columns(); i++) {
        if (!context->is_constant_column(i)) {
            state->list_all_const = false;
            break;
        }
    }

    if (state->list_all_const) {
        if (context->is_constant_column(0)) {
            state->all_const = true;
        }
        for (int i = 1; i < context->get_num_args(); i++) {
            const auto list_col = context->get_constant_column(i);
            if (list_col->only_null()) {
                continue;
            } else {
                const auto list_val = ColumnHelper::get_const_value<Type>(list_col);
                state->mp.emplace(list_val, i);
            }
        }
    }

    return Status::OK();
}

template <LogicalType Type>
Status StringFunctions::field_close(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return Status::OK();
    }

    auto* state = reinterpret_cast<FieldFuncState<Type>*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
    delete state;

    return Status::OK();
}

template <LogicalType Type>
StatusOr<ColumnPtr> StringFunctions::field(FunctionContext* context, const Columns& columns) {
    auto size = columns[0]->size();
    ColumnBuilder<TYPE_INT> result(size);
    const FieldFuncState<Type>* state =
            reinterpret_cast<const FieldFuncState<Type>*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
    if (columns[0]->only_null()) {
        result.append(0);
        return result.build(true);
    } else if (state != nullptr) {
        if (state->all_const) {
            const auto list_col = context->get_constant_column(0);
            const auto list_val = ColumnHelper::get_const_value<Type>(list_col);
            auto it = state->mp.find(list_val);
            if (it != state->mp.end()) {
                result.append(it->second);
            } else {
                result.append(0);
            }
            return result.build(true);
        } else if (state->list_all_const) {
            const auto viewer = ColumnViewer<Type>(columns[0]);
            for (int i = 0; i < size; i++) {
                const auto& list_val = viewer.value(i);
                auto it = state->mp.find(list_val);
                if (it != state->mp.end()) {
                    result.append(it->second);
                } else {
                    result.append(0);
                }
            }
            return result.build(false);
        }
    }

    std::vector<ColumnViewer<Type>> list;
    list.reserve(columns.size());
    for (const ColumnPtr& col : columns) {
        list.emplace_back(ColumnViewer<Type>(col));
    }

    for (int row = 0; row < size; row++) {
        auto value = list[0].value(row);
        int res = 0, id = 1;
        for (auto it = std::next(list.begin()); it != list.end(); it++) {
            if (!it->is_null(row) && value == it->value(row)) {
                res = id;
                break;
            }
            id++;
        }

        result.append(res);
    }

    return result.build(ColumnHelper::is_all_const(columns));
}

} // namespace starrocks
