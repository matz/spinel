# Comparable, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name for a CONCRETE
# Integer, Bignum or Float receiver: the compiler has no C emitter for that
# receiver shape any more. String and a user class with its own `<=>` are
# NOT covered (see analyze_desugar.c's SP_BX_COMPARABLE receiver rule for
# the two separate pre-existing gaps that ruled them out this pass). A
# run-time-typed (poly) receiver is
# NOT rewritten onto these definitions: it keeps using the existing
# runtime dispatch (sp_num_clamp and friends, lib/spinel_rt.h) -- an
# is_a?-split rewrite was measured at 1.7x-3.5x the cost of that dispatch
# for Integer/Float's own methods (see builtins/integer.rb's header), and
# Comparable's receiver set is wider still, so the same trade applies here
# without needing to re-measure.
#
# A user class that defines its own `<=>` is ALSO deliberately NOT
# rewritten onto these definitions, despite the brief asking for it: doing
# so writes `lo <=> hi` (and `self <=> lo`) in Ruby with the operands
# concretely that class, which is a genuinely NEW kind of call site for
# the class's OWN `<=>` method -- every existing route to it (the
# `<`/`>`/`between?` operators, `sort`/`min`/`max`, the pre-existing
# object-clamp C emitter itself) calls it through the boxed runtime hook
# (sp_obj_cmp_hook), never as a plain, statically-typed Ruby expression.
# Adding that one concretely-typed call site changed how the analyzer
# settles the method's OWN parameter type, and a program that also uses
# the SAME `<=>` with a DIFFERENT argument type elsewhere (any
# `x.clamp(range)`, whose old C emitter calls `<=>` with an Integer
# endpoint through the very same hook) miscompiled: the parameter stayed
# typed as the user's class while a real Integer argument flowed into it
# at runtime, reading a bogus field through a pointer that was never one
# (test/comparable_clamp_parity.rb, a pre-existing test, reproduces
# the underlying gap with ZERO Comparable/clamp Ruby migration involved --
# a bare `a <=> b` beside an unrelated `x.clamp(1..5)` already miscompiles
# on the unmigrated compiler; this migration would merely have given the
# first such call site a reason to exist for a class that never had one).
# This is a general, pre-existing method-typing gap (a method's parameter
# type has to account for every REACHABLE caller, hook-based ones
# included), not something to paper over inside one migration; see
# analyze_desugar.c's own comment on the SP_BX_COMPARABLE receiver rule
# for where the exclusion lives, and the eighth pass's policy proposal for
# the recommended follow-up.
#
# `between?` and the two-ARGUMENT form of `clamp` are both defined purely
# on `<=>`, exactly as CRuby's Comparable module is: `<=>`, `==`, `is_a?`
# and `nil?` are all generic operators that already answer correctly for
# ANY concrete argument type at a given call site's clone (unlike a raw
# arithmetic operator or a plain method call such as `-@`/`.div`, which
# fail to COMPILE for a required parameter whose concrete type has no
# such operation -- see builtins/integer.rb's gcd/ceildiv comments), so
# neither method needs an is_a?-narrowing dance of its own.
#
# `clamp` ALSO takes a single Range argument, deliberately NOT covered by
# this file: `def clamp(lo, hi)` here has exactly two required parameters,
# so the desugar mechanism's own arity gate (analyze_desugar.c's
# desugar_builtin_scalar_calls checks a call site's argument count against
# the generic def's requireds/optionals) leaves a one-argument `clamp`
# call on the UNCHANGED, pre-existing C emitter entirely -- a real
# regression, not a design choice, is why: a `def clamp(a, b =
# <sentinel>)` single method covering BOTH shapes forces `b`'s own
# parameter type to unify across the sentinel default's type (a Symbol)
# and whatever type a real second argument has, for EVERY call site
# regardless of which shape it uses (Ruby's optional-parameter default
# lives in the same function body as the passed-argument case, unlike a
# per-call-site-cloned REQUIRED parameter) -- boxing `clamp(1, 10)`'s
# plain Integer bound into an sp_RbVal on every call, measured at 13x the
# old emitter's cost on a 50,000,000-call bench of exactly that shape.
# Splitting the Range form into its own sibling method the two-argument
# form calls (mirroring builtins/integer.rb's lcm/gcd) avoided the box but
# hit a SEPARATE wall: that sibling's own body calls a second sibling two
# clone-generations deep, and the per-call-site cloning fixpoint did not
# converge on it in the rounds it runs, reaching codegen still unrewritten
# and raising NoMethodError. Leaving the Range form on its own working,
# already-correct C emitter is a strictly safer trade than either.
module Comparable
  def between?(min, max)
    c1 = self <=> min
    if c1.nil?
      raise ArgumentError, "comparison of #{self.class} with #{__cmp_repr(min)} failed"
    end
    c2 = self <=> max
    if c2.nil?
      raise ArgumentError, "comparison of #{self.class} with #{__cmp_repr(max)} failed"
    end
    c1 >= 0 && c2 <= 0
  end

  def clamp(lo, hi)
    # a nil bound is an open (unbounded) side on that end, so it takes no
    # part in the ordering check or in either comparison against self.
    if !lo.nil? && !hi.nil?
      c = lo <=> hi
      if c.nil?
        raise ArgumentError, "comparison of #{lo.class} with #{__cmp_repr(hi)} failed"
      end
      # a STRICT ordering violation only: equal bounds are a valid (empty)
      # range, and `clamp` answers the receiver or that shared bound.
      if c > 0
        raise ArgumentError, "min argument must be less than or equal to max argument"
      end
    end
    unless lo.nil?
      c = self <=> lo
      if c.nil?
        raise ArgumentError, "comparison of #{self.class} with #{__cmp_repr(lo)} failed"
      end
      return lo if c < 0
    end
    unless hi.nil?
      c = self <=> hi
      if c.nil?
        raise ArgumentError, "comparison of #{self.class} with #{__cmp_repr(hi)} failed"
      end
      return hi if c > 0
    end
    self
  end

  # CRuby's rb_cmperr names the failing bound by its VALUE only for an
  # immediate-ish type (an Integer that fits a machine word, a Float, a
  # Symbol, nil, true or false) and by its CLASS NAME for everything else,
  # a Bignum included ("comparison of Integer with String failed", not
  # "...with z failed"; a huge Bignum bound answers its class name too,
  # not its own digits -- a corner this generic Ruby form does not
  # reproduce, since Integer and Bignum are the one class from Ruby's own
  # side; the C runtime helper this file's poly receivers still use
  # (sp_poly_cmp_err_repr/sp_cmperr_desc, lib/spinel_rt.h) keeps the exact
  # distinction via the value's tag). The RECEIVER side of the message is
  # always the plain class name, never inspected.
  def __cmp_repr(v)
    if v.is_a?(Integer) || v.is_a?(Float) || v.is_a?(Symbol) || v.nil? ||
       v.is_a?(TrueClass) || v.is_a?(FalseClass)
      v.inspect
    else
      v.class.name
    end
  end
end
