# Integer, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name for a CONCRETE
# Integer/Bignum receiver: the compiler has no C emitter for that receiver
# shape any more. A program that mentions one of these names has this file
# spliced ahead of it (spinel_parse.c's sp_splice_builtin_extras), and the
# analyzer rewrites every definition into a top-level function that takes
# the receiver as its first parameter, the same mechanism
# builtins/enumerable.rb uses (analyze_desugar.c, desugar_builtin_scalar_defs
# / desugar_builtin_scalar_calls). A run-time-typed (poly) receiver is NOT
# rewritten onto these definitions: it keeps using the existing runtime
# dispatch (sp_poly_int_*, lib/spinel_rt.h), which already answers every
# name below correctly for both a small Integer and a Bignum -- see the
# mechanism's own comment in analyze_desugar.c for why duplicating that
# split was not worth the added rewrite surface.
#
# A method that is never called never reaches the generated C. Write these
# in the Ruby the compiler compiles: plain locals, no reflection, no block.

class Integer
  def digits(base = 10)
    # CRuby's C implementation coerces `base` up front (rb_to_int), raising
    # this exact TypeError for anything that is not already an Integer,
    # before ever comparing it; a plain `base < 0` here would instead ask
    # nil/a String to answer `<`, giving a different exception entirely
    # (NoMethodError / a comparison-failed ArgumentError). Written out
    # explicitly so the two match (test/numeric_nil_argument.rb,
    # test/numeric_string_argument.rb, test/strict_arg_conversion.rb).
    # Float bases truncate through their own to_int before this check.
    base = base.to_int if base.is_a?(Float)
    unless base.is_a?(Integer)
      raise TypeError, "no implicit conversion of #{base.nil? ? "nil" : base.class} into Integer"
    end
    raise ArgumentError, "negative radix" if base < 0
    raise ArgumentError, "invalid radix #{base}" if base < 2
    raise Math::DomainError, "out of domain" if self < 0
    return [0] if self == 0
    result = []
    n = self
    while n > 0
      result << n % base
      n /= base
    end
    result
  end

  def bit_length
    # A linear shift-and-count loop measured 5x the cost of the C emitter's
    # binary-search reduction (a 200,000,000-call bench, well past the
    # ~10% bound); a first attempt at the same doubling technique
    # (checking against a 2**64 literal to fold off 64 bits at once) was
    # 20x worse still -- comparing an Integer-typed local against a
    # Bignum-sized literal anywhere in this body widened every use of
    # that local for the whole function, including the receiver's own
    # fast path, onto boxed/Bignum arithmetic. Every literal below fits
    # the target's Integer, so an Integer receiver's shifts stay native.
    # Measured faster than the old emitter once compiled into the same
    # translation unit (0.21s vs 0.31s, 200,000,000 calls) -- the library
    # call it replaces could never be inlined across the .a boundary the
    # way this generated copy is.
    #
    # The loop reduces SIXTEEN bits per pass, not thirty-two: sp_int is
    # the pointer width, so on a 32-bit target `n >> 32` is a shift past
    # the width of the type, which the C compiler refuses under -Werror
    # (`right shift count >= width of type`) and which no Integer
    # receiver there could need anyway. Three passes cover an int64's
    # magnitude, and a small receiver takes none.
    n = self < 0 ? ~self : self
    b = 0
    while n >= 65536
      b += 16
      n = n >> 16
    end
    if n >= 256 then b += 8; n = n >> 8 end
    if n >= 16 then b += 4; n = n >> 4 end
    if n >= 4 then b += 2; n = n >> 2 end
    if n >= 2 then b += 1; n = n >> 1 end
    b + n
  end

  def gcd(other)
    # CRuby rejects anything but an Integer here, Float included (even a
    # whole one), with this exact message, no interpolated class name.
    # The if/else form matters, not just style: a guard-clause shape
    # (`raise X unless y.is_a?(Integer)` then using `y` normally after)
    # does not stop a call site whose actual argument is e.g. an Array
    # from specializing this clone with `other` typed concretely Array,
    # and `other < 0` two lines down then fails to COMPILE
    # (`undefined method '<' for an instance of Array`) even though it
    # is unreachable at run time. Nesting the body inside the true arm
    # of the is_a? check itself (this shape) does not have the problem
    # -- caught by test/integer_gcd_arg_check.rb, which the guard-clause
    # draft of this method failed outright.
    if other.is_a?(Integer)
      a = self < 0 ? -self : self
      b = other < 0 ? -other : other
      while b != 0
        a, b = b, a % b
      end
      a
    else
      raise TypeError, "not an integer"
    end
  end

  def lcm(other)
    if other.is_a?(Integer)
      if self == 0 || other == 0
        0
      else
        g = gcd(other)
        a = self < 0 ? -self : self
        b = other < 0 ? -other : other
        (a / g) * b
      end
    else
      raise TypeError, "not an integer"
    end
  end

  def gcdlcm(other)
    if other.is_a?(Integer)
      [gcd(other), lcm(other)]
    else
      raise TypeError, "not an integer"
    end
  end

  def ceildiv(other)
    # CRuby's own algorithm (found by black-box probing a coerce-tracing
    # stub, since there is no source to read here): negate `other` FIRST
    # (a real call to its own unary `-@`, which is why a receiver lacking
    # one -- Array, Hash, Symbol, nil, true, false -- answers CRuby's
    # "undefined method `-@'" rather than a coercion error), floor-divide,
    # then negate the quotient. `elsif is_a?(Float)` (not a single
    # `is_a?(Numeric)` arm) because the two need the SAME body but each
    # needs its own is_a? to narrow `other` for codegen: a single shared
    # arm left `other` at the call site's own concrete type in the arm
    # CRuby ALSO takes, and a concrete Array/Hash/String argument (a call
    # CRuby raises for at run time, not reject at compile time) has no
    # `-@`/`div` to bind and failed to COMPILE outright -- the same
    # REQUIRED-parameter pitfall gcd's own commit found, one narrowing
    # arm per accepted type rather than gcd's single is_a? guard. The
    # final `else` never touches `other` itself, so it compiles for any
    # type; CRuby's own message there is class-specific (NoMethodError
    # naming `-@`) but this is what the unmigrated compiler already
    # answered for the same inputs (`5.gcd(other)`'s sibling arms took
    # the same simplification), so this is not a new gap.
    if other.is_a?(Integer)
      -(self.div(-other))
    elsif other.is_a?(Float)
      -(self.div(-other))
    elsif other.is_a?(Rational)
      # exactly, as CRuby does: Integer#div of a Rational floors the exact
      # quotient (a typed Rational, a boxed one and a big one alike), where
      # this arm's absence raised TypeError for every Rational divisor. The
      # quotient is held in a local before it is negated: a Bignum's `-@` is
      # 0 - q, and the 0 it allocates first was collected while `-other`
      # allocated a Rational (a garbage answer under SPINEL_GC_STRESS=1).
      q = self.div(-other)
      -q
    else
      raise TypeError, "not an integer"
    end
  end

  def remainder(other)
    # The sign follows the RECEIVER, not the divisor (`%`/modulo's own
    # rule) -- (-7).remainder(3) is -1, (-7) % 3 is 2. `%` already floors
    # correctly for Integer/Float/Bignum/Rational, and correctly runs the
    # #coerce protocol for a user object that defines it (its own
    # migration is not this pass's job), so remainder is `%`'s answer
    # corrected back to a truncated sign convention: when the floored
    # remainder is non-zero and its sign disagrees with the receiver's,
    # subtracting the divisor once gives the truncated-division remainder
    # CRuby answers, without a second division or a float round-trip that
    # would lose precision on a Bignum receiver.
    #
    # NOT the is_a?(Integer)/is_a?(Float) shape ceildiv/gcd use: `%` (the
    # raw operator, unlike `.div`/`-@` as plain method calls) does not
    # fail to COMPILE for a REQUIRED parameter whose concrete type has no
    # numeric meaning at all -- it silently miscompiles instead (`f(x, y)
    # = x % y` called with a String/Array/Hash/nil/true/false-typed `y`
    # answers a wrong number or a bogus ZeroDivisionError, never a
    # TypeError, confirmed identical and pre-existing on the unmigrated
    # compiler). A first draft gated this the ceildiv way (is_a?(Integer)
    # / is_a?(Float) / else raise) and it silently broke the numeric
    # coerce protocol instead: `5.modulo(Num.new)` (Num#coerce defined)
    # is neither Integer nor Float, so a same-shaped `modulo` fell into
    # the "else" and raised, where CRuby (and `%` itself, called
    # directly) coerces and answers 2.0 (test/numeric_coerce_protocol.rb,
    # caught before landing). Excluding exactly the closed set of types
    # `%` cannot handle -- nil/true/false/Symbol/String/Array/Hash -- and
    # letting everything else (Integer, Float, Bignum, Rational, a
    # coercible or plain user object) reach `%` directly keeps both
    # correct: `%`'s own dispatch already raises properly for a user
    # object with no coerce.
    #
    # Known pre-existing divergence, unchanged by this migration: CRuby's
    # C implementation orders its OWN sign check before the modulo,
    # comparing the raw uncoerced `other` against 0 -- so `Num` above
    # (coerce only, no `<=>`) makes `5.remainder(Num.new)` raise
    # ArgumentError ("comparison of Num with 0 failed") in real CRuby.
    # Comparing `other` here instead of `%`'s already-coerced answer
    # would match that, but `<` (a plain method call, unlike `%`) then
    # fails to COMPILE for the same required-parameter reason `.div`/
    # `-@` do in ceildiv/gcd, for a concrete Array/Hash-typed call site.
    # Comparing the post-modulo VALUE (`r < 0`, a real number by
    # construction) compiles for every type and answers 2.0 for this one
    # exotic shape instead of raising -- identical to the unmigrated
    # compiler's own answer here (confirmed on `Num`), so not a new gap.
    if other.nil? || other == true || other == false || other.is_a?(Symbol) ||
       other.is_a?(String) || other.is_a?(Array) || other.is_a?(Hash)
      if other.nil? || other == true || other == false || other.is_a?(Symbol)
        raise TypeError, "#{other.inspect} can't be coerced into Integer"
      else
        raise TypeError, "#{other.class} can't be coerced into Integer"
      end
    else
      r = self % other
      (r != 0 && (r < 0) != (self < 0)) ? r - other : r
    end
  end

  def fdiv(other)
    # Always a Float, never raising (7.fdiv(0) is Infinity, matching
    # IEEE754 float division, not ZeroDivisionError): `to_f / other`
    # converts the receiver once (a Bignum receiver loses precision the
    # same way CRuby's own conversion does) and leaves the division to
    # `/`, which already runs the numeric #coerce protocol for a user
    # object and handles Rational/Float/Integer/Bignum arguments
    # correctly. Same exclusion-list shape as remainder, not
    # is_a?(Integer)/is_a?(Float): `/` (the raw operator) silently
    # miscompiles rather than failing to compile for a REQUIRED
    # parameter of a String/Array/Hash/nil/true/false concrete type
    # (confirmed via `def f(x, y) = x.to_f / y`, identical and
    # pre-existing on the unmigrated compiler), so those seven types are
    # excluded by name and everything else reaches `/` directly. The
    # message says "into Integer", not "into Float" as `x.to_f / y`
    # alone would answer: CRuby's own Integer#fdiv coerces the argument
    # by that name before ever converting to a Float, verified against
    # a literal `7.fdiv(nil)` etc on real CRuby.
    #
    # A genuine engine gap surfaced writing this, fixed in
    # analyze_infer.c: an Integer/Bignum arith op with a non-coercible
    # argument (String/Symbol/nil/bool/Array/Hash/Range) was already
    # typed as the raising expression's own kind so it could sit in a
    # value position (#2471) -- but only for an Integer/Bignum RECEIVER,
    # not a Float one, even though codegen_call.c's own matching arm
    # (#3645) already emits the Float-side raise correctly. Every method
    # here that raises inside an is_a? branch (gcd, ceildiv, remainder)
    # happened to keep a scalar return type across every clone anyway,
    # so this never mattered until fdiv's `self.to_f / other`: for a
    # call site whose argument is one of those excluded types, this
    # exact expression is unreachable but still has to type as SOMETHING
    # other than UNKNOWN -- UNKNOWN poisoned the whole clone's return
    # type to void, and every caller of `7.fdiv([1, 2])`-shaped code
    # failed to compile ("void value not ignored"). Added the missing
    # TY_FLOAT arm right next to the existing TY_INT/TY_BIGINT one.
    if other.nil? || other == true || other == false || other.is_a?(Symbol) ||
       other.is_a?(String) || other.is_a?(Array) || other.is_a?(Hash)
      if other.nil? || other == true || other == false || other.is_a?(Symbol)
        raise TypeError, "#{other.inspect} can't be coerced into Integer"
      else
        raise TypeError, "#{other.class} can't be coerced into Integer"
      end
    else
      self.to_f / other
    end
  end
end
