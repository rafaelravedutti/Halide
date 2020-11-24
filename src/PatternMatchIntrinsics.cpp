#include "PatternMatchIntrinsics.h"
#include "CodeGen_Internal.h"
#include "ConciseCasts.h"
#include "CSE.h"
#include "IRMatch.h"
#include "IRMutator.h"
#include "Simplify.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using namespace Halide::ConciseCasts;

namespace {

Expr widen(Expr a) {
    return Cast::make(a.type().with_bits(a.type().bits() * 2), std::move(a));
}

Expr narrow(Expr a) {
    return Cast::make(a.type().with_bits(a.type().bits() / 2), std::move(a));
}

Expr saturating_narrow(Expr a) {
    Type narrow = a.type().with_bits(a.type().bits() / 2);
    return saturating_cast(narrow, a);
}

struct Pattern {
    Expr pattern;
    Call::IntrinsicOp replacement;
};

Expr apply_patterns(Type type, Expr x, const std::vector<Pattern> &patterns) {
    std::vector<Expr> matches;
    for (const Pattern &i : patterns) {
        if (expr_match(i.pattern, x, matches)) {
            return Call::make(type, i.replacement, matches, Call::PureIntrinsic);
        }
    }
    return Expr();
}

Expr find_and_subtract(const Expr &e, const Expr &term) {
    if (const Add *add = e.as<Add>()) {
        Expr a = find_and_subtract(add->a, term);
        if (!a.same_as(add->a)) {
            return Add::make(a, add->b);
        }
        Expr b = find_and_subtract(add->b, term);
        if (!b.same_as(add->b)) {
            return Add::make(add->a, b);
        }
    } else if (can_prove(e == term)) {
        return make_zero(e.type());
    }
    return e;
}

// Perform peephole optimizations on the IR, adding appropriate
// interleave and deinterleave calls.
class PatternMatchIntrinsics : public IRMutator {
protected:
    using IRMutator::visit;

    IRMatcher::Wild<0> x;
    IRMatcher::Wild<1> y;
    IRMatcher::Wild<2> z;
    IRMatcher::Wild<3> w;
    IRMatcher::Wild<4> u;
    IRMatcher::Wild<5> v;
    IRMatcher::WildConst<0> c0;
    IRMatcher::WildConst<1> c1;
    IRMatcher::WildConst<2> c2;
    IRMatcher::WildConst<3> c3;
    IRMatcher::WildConst<4> c4;
    IRMatcher::WildConst<5> c5;

    Expr visit(const Add *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        for (halide_type_code_t code : {op->type.code(), halide_type_uint}) {
            Type narrow = op->type.with_bits(op->type.bits() / 2).with_code(code);
            Expr narrow_a = lossless_cast(narrow, a);
            Expr narrow_b = lossless_cast(narrow, b);

            if (narrow_a.defined() && narrow_b.defined()) {
                Expr result = widening_add(narrow_a, narrow_b);
                if (result.type() != op->type) {
                    result = Cast::make(op->type, result);
                }
                return result;
            }
        }

        if (a.same_as(op->a) && b.same_as(op->b)) {
            return op;
        } else {
            return Add::make(a, b);
        }
    }

    Expr visit(const Sub *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        for (halide_type_code_t code : {halide_type_int, halide_type_uint}) {
            Type narrow = op->type.with_bits(op->type.bits() / 2).with_code(code);
            Expr narrow_a = lossless_cast(narrow, a);
            Expr narrow_b = lossless_cast(narrow, b);

            if (narrow_a.defined() && narrow_b.defined()) {
                Expr result = widening_subtract(narrow_a, narrow_b);
                if (result.type() != op->type) {
                    result = Cast::make(op->type, result);
                }
                return result;
            }
        }

        if (a.same_as(op->a) && b.same_as(op->b)) {
            return op;
        } else {
            return Sub::make(a, b);
        }
    }

    Expr visit(const Mul *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        // We're applying this to float, which seems OK? float16 * float16 -> float32 is a widening multiply?
        Type narrow = op->type.with_bits(op->type.bits() / 2);
        Expr narrow_a = lossless_cast(narrow, a);
        Expr narrow_b = lossless_cast(narrow, b);

        if (narrow_a.defined() && narrow_b.defined()) {
            return widening_multiply(narrow_a, narrow_b);
        }

        if (a.same_as(op->a) && b.same_as(op->b)) {
            return op;
        } else {
            return Mul::make(a, b);
        }
    }

    Expr visit(const Div *op) override {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        if (!op->type.is_float()) {
            return mutate(lower_int_uint_div(a, b));
        }

        if (a.same_as(op->a) && b.same_as(op->b)) {
            return op;
        } else {
            return Div::make(a, b);
        }
    }

    Expr visit(const Cast *op) override {
        Expr value = mutate(op->value);

        if ((op->type.is_int() || op->type.is_uint()) && op->type.bits() > 1 && op->type.bits() <= 32) {
            Expr lower = op->type.min();
            Expr upper = op->type.max();
/*
            auto rewrite = IRMatcher::rewriter(IRMatcher::cast(op->type, value), op->type);
            using IRMatcher::intrin;
            if (rewrite(max(min(intrin(Call::widening_add, x, y), c1), c0), intrin(Call::saturating_add, x, y)) ||
                rewrite(min(max(intrin(Call::widening_add, x, y), c0), c1), intrin(Call::saturating_add, x, y)) ||
                rewrite(max(min(intrin(Call::widening_subtract, x, y), c1), c0), intrin(Call::saturating_subtract, x, y)) ||
                rewrite(min(max(intrin(Call::widening_subtract, x, y), c0), c1), intrin(Call::saturating_subtract, x, y)) ||
                // These are only correct for unsigned types.
                rewrite(min(intrin(Call::widening_add, x, y), c1), intrin(Call::saturating_add, x, y), c1 == upper) ||
                rewrite(max(intrin(Call::widening_subtract, x, y), c0), intrin(Call::saturating_subtract, x, y), c0 == lower) ||

                // Averaging/halving add/subtract.
                //rewrite(intrin(Call::widening_add, x, y) >> 1, intrin(Call::halving_add, x, y)) ||
                //rewrite(intrin(Call::widening_subtract, x, y) >> 1, intrin(Call::halving_subtract, x, y)) ||
                //{ widening_subtract(w, w) >> 1, Call::halving_subtract },
                //{ rounding_shift_right(widening_add(w, w), 1), Call::rounding_halving_add },
                // This is only correct for signed types.
                //{ rounding_shift_right(widening_subtract(sw, sw), 1), Call::rounding_halving_subtract },
                false) {
                return rewrite.result;
            }
*/
            Expr w(Variable::make(op->type, "*"));
            Expr sw(Variable::make(op->type.with_code(halide_type_int), "*"));
            Expr uw(Variable::make(op->type.with_code(halide_type_uint), "*"));

            std::vector<Pattern> patterns = {
                // Saturating add/subtract
                Pattern{max(min(widening_add(w, w), upper), lower), Call::saturating_add},
                Pattern{min(max(widening_add(w, w), lower), upper), Call::saturating_add},
                Pattern{max(min(widening_subtract(w, w), upper), lower), Call::saturating_subtract},
                Pattern{min(max(widening_subtract(w, w), lower), upper), Call::saturating_subtract},
                // These are only correct for unsigned types.
                Pattern{min(widening_add(uw, uw), upper), Call::saturating_add},
                Pattern{max(widening_subtract(uw, uw), lower), Call::saturating_subtract},

                // Averaging/halving add/subtract.
                Pattern{widening_add(w, w) >> 1, Call::halving_add},
                Pattern{widening_subtract(w, w) >> 1, Call::halving_subtract},
                Pattern{rounding_shift_right(widening_add(w, w), 1), Call::rounding_halving_add},
                // This is only correct for signed types.
                Pattern{rounding_shift_right(widening_subtract(sw, sw), 1), Call::rounding_halving_subtract},
            };

            Expr result = apply_patterns(op->type, value, patterns);
            if (result.defined()) {
                //internal_error << "expr_match worked but IRMatcher did not!\n" << Expr(op) << "\n" << result << "\n";
                return result;
            }
        }

        if (value.same_as(op->value)) {
            return op;
        } else {
            return Cast::make(op->type, value);
        }
    }

    Expr visit(const Call *op) override {
        if (op->is_intrinsic(Call::shift_right) || op->is_intrinsic(Call::shift_left)) {
            internal_assert(op->args.size() == 2);
            Expr a = mutate(op->args[0]);
            Expr b = mutate(op->args[1]);

            // Match rounding_shift_right(a, b) = (widen(a) + ((1 << b) / 2)) >> b
            Expr round;
            if (op->is_intrinsic(Call::shift_right)) {
                round = (make_const(a.type(), 1) << max(b, 0)) >> 1;
            } else {
                round = (make_const(a.type(), 1) >> min(b, 0)) >> 1;
            }
            round = simplify(round);

            Expr a_less_round = find_and_subtract(a, round);
            if (!a_less_round.same_as(a)) {
                a_less_round = simplify(a_less_round);
                if (op->is_intrinsic(Call::shift_right)) {
                    return mutate(rounding_shift_right(a_less_round, b));
                } else {
                    return mutate(rounding_shift_left(a_less_round, b));
                }
            }

            if (a.same_as(op->args[0]) && b.same_as(op->args[1])) {
                return op;
            } else if (op->is_intrinsic(Call::shift_right)) {
                return Call::make(op->type, Call::shift_right, {a, b}, Call::PureIntrinsic);
            } else {
                return Call::make(op->type, Call::shift_left, {a, b}, Call::PureIntrinsic);
            }
        } else {
            return op;
        }
    }
};

}  // namespace

Stmt pattern_match_intrinsics(Stmt s) {
    s = substitute_in_all_lets(s);
    s = PatternMatchIntrinsics().mutate(s);
    s = common_subexpression_elimination(s);
    return s;
}

Expr lower_widening_add(Expr a, Expr b) {
    return widen(a) + widen(b);
}

Expr lower_widening_subtract(Expr a, Expr b) {
    Type wide = a.type().with_bits(a.type().bits() * 2);
    if (wide.is_uint()) {
        wide = wide.with_code(halide_type_int);
    }
    return cast(wide, a) - cast(wide, b);
}

Expr lower_widening_multiply(Expr a, Expr b) {
    return widen(a) * widen(b);
}


Expr lower_rounding_shift_right(Expr a, Expr b) {
    Expr round = simplify((make_const(a.type(), 1) << max(b, 0)) >> 1);
    // This implementating of rounding shift overflows! But it matches a lot of old behavior.
    a = simplify(a + round);
    return Call::make(a.type(), Call::shift_right, {a, b}, Call::PureIntrinsic);
}

Expr lower_rounding_shift_left(Expr a, Expr b) {
    Expr round = simplify((make_const(a.type(), 1) >> min(b, 0)) >> 1);
    // This implementating of rounding shift overflows! But it matches a lot of old behavior.
    a = simplify(a + round);
    return Call::make(a.type(), Call::shift_left, {a, b}, Call::PureIntrinsic);
}

// These intentionally use the non-lowered versions of widening_add/widening_subtract, in the
// hopes that maybe the user of this will be able to use the information. If not, it will
// probably recursively call lower_widening_add/lower_widening_subtract.
Expr lower_saturating_add(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    return saturating_narrow(widening_add(a, b));
}
Expr lower_saturating_subtract(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    return saturating_cast(a.type(), widening_subtract(a, b));
}

Expr lower_halving_add(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    Expr result_2x = widening_add(a, b);
    return Cast::make(a.type(), result_2x >> 1);
}

Expr lower_rounding_halving_add(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    Expr result_2x = widening_add(a, b);
    return Cast::make(a.type(), rounding_shift_right(result_2x, 1));
}

Expr lower_halving_subtract(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    Expr result_2x = widening_subtract(a, b);
    return Cast::make(a.type(), result_2x >> 1);
}

Expr lower_rounding_halving_subtract(Expr a, Expr b) {
    internal_assert(a.type() == b.type());
    Expr result_2x = widening_subtract(a, b);
    return Cast::make(a.type(), rounding_shift_right(result_2x, 1));
}

Expr lower_intrinsic(const Call *op) {
    if (op->is_intrinsic(Call::widening_add)) {
        internal_assert(op->args.size() == 2);
        return lower_widening_add(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::widening_subtract)) {
        internal_assert(op->args.size() == 2);
        return lower_widening_subtract(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::widening_multiply)) {
        internal_assert(op->args.size() == 2);
        return lower_widening_multiply(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::rounding_shift_right)) {
        internal_assert(op->args.size() == 2);
        return lower_rounding_shift_right(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::rounding_shift_left)) {
        internal_assert(op->args.size() == 2);
        return lower_rounding_shift_left(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::halving_add)) {
        internal_assert(op->args.size() == 2);
        return lower_halving_add(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::halving_subtract)) {
        internal_assert(op->args.size() == 2);
        return lower_halving_subtract(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::rounding_halving_add)) {
        internal_assert(op->args.size() == 2);
        return lower_rounding_halving_add(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::rounding_halving_subtract)) {
        internal_assert(op->args.size() == 2);
        return lower_rounding_halving_subtract(op->args[0], op->args[1]);
    } else if (op->is_intrinsic(Call::mulhi_shr)) {
        internal_assert(op->args.size() == 3);

        Type ty = op->type;
        Type wide_ty = ty.with_bits(ty.bits() * 2);

        Expr p_wide = cast(wide_ty, op->args[0]) * cast(wide_ty, op->args[1]);
        const UIntImm *shift = op->args[2].as<UIntImm>();
        internal_assert(shift != nullptr)
            << "Third argument to mulhi_shr intrinsic must be an unsigned integer immediate.\n";
        return cast(ty, p_wide >> (shift->value + ty.bits()));
    } else if (op->is_intrinsic(Call::sorted_avg)) {
        internal_assert(op->args.size() == 2);
        // b > a, so the following works without widening:
        // a + (b - a)/2
        return op->args[0] + (op->args[1] - op->args[0]) / 2;
    } else if (op->is_intrinsic(Call::abs)) {
        // Generate select(x >= 0, x, -x) instead
        std::string x_name = unique_name('x');
        Expr x = Variable::make(op->args[0].type(), x_name);
        return Let::make(x_name, op->args[0], select(x >= 0, x, -x));
    } else if (op->is_intrinsic(Call::absd)) {
        // Use a select instead
        std::string a_name = unique_name('a');
        std::string b_name = unique_name('b');
        Expr a_var = Variable::make(op->args[0].type(), a_name);
        Expr b_var = Variable::make(op->args[1].type(), b_name);
        return Let::make(a_name, op->args[0],
                          Let::make(b_name, op->args[1],
                                    Select::make(a_var < b_var, b_var - a_var, a_var - b_var)));
    } else {
        internal_error << "Unknown intrinsic " << op->name;
        return Expr();
    }
}

}  // namespace Internal
}  // namespace Halide
