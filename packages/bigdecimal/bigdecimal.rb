# A minimal BigDecimal for Spinel (#4881): an exact decimal, in pure Ruby.
#
# The value is sign * mantissa * 10 ** exponent, the mantissa an array of
# decimal digits (least significant first). Digits rather than one Integer:
# a mantissa outgrows a 64-bit Integer at 19 digits, and Spinel's Integer
# only grows past that under --int-overflow=promote; this works in every mode.
#
# Covered: BigDecimal(Integer), BigDecimal(String) (decimal and exponent
# forms), + - * / with a BigDecimal, Integer or Float on either side,
# comparison (<=>, ==, <, >, clamp through Comparable), -@, abs, zero?,
# negative?, positive?, to_f, to_i, to_s / inspect in CRuby's "0.xxxen"
# form, Math.sqrt through to_f (a Float, as in CRuby), the seven rounding
# constants (ROUND_UP, ROUND_DOWN, ROUND_HALF_UP, ROUND_HALF_DOWN,
# ROUND_HALF_EVEN, ROUND_CEILING, ROUND_FLOOR, with CRuby's integer values)
# and #round(n) / #round(n, mode) for a constant or its Symbol. Every mode
# is computed on the digit array, so it is exact and never goes through a
# Float.
#
# Division carries DIV_DIGITS significant digits, rounding half up (CRuby's
# default mode). CRuby picks the precision per operation from the operands'
# sizes; to_f of a result agrees with CRuby's to the Float's precision, not
# necessarily in the last decimal digit of to_s.
#
# Not yet: a Float argument to BigDecimal() (CRuby requires a precision for
# it), precision arguments, #sqrt, NaN and Infinity. BigDecimal.mode and the
# rounding_mode accessors are absent, so the default mode (half up) is the
# only one #round uses when no mode is given; the `half:` keyword form
# (round(half: :even)) is not modelled. A BigDecimal result of rounding a
# negative value down to zero is CRuby's signed -0.0, which sign*mantissa
# cannot represent, so #round raises there rather than answer +0.0. Without
# Infinity, a division by zero raises ZeroDivisionError where CRuby's
# default mode answers Infinity.
class BigDecimal < Numeric
  include Comparable

  DIV_DIGITS = 40

  # CRuby's BigDecimal::ROUND_* values, and the internal names its Symbols
  # map to (the same numbers). #round takes either.
  ROUND_UP = 1
  ROUND_DOWN = 2
  ROUND_HALF_UP = 3
  ROUND_HALF_DOWN = 4
  ROUND_CEILING = 5
  ROUND_FLOOR = 6
  ROUND_HALF_EVEN = 7

  # sign: -1, 0 or 1; digits: least significant first, no leading or
  # trailing zero (a zero is [] with sign 0)
  def initialize(sign, digits, exp)
    lo = 0
    lo += 1 while lo < digits.length && digits[lo] == 0
    hi = digits.length
    hi -= 1 while hi > lo && digits[hi - 1] == 0
    if lo >= hi
      @sign = 0
      @digits = []
      @exp = 0
    else
      @sign = sign
      @digits = digits[lo...hi]
      @exp = exp + lo
    end
  end

  def __sign = @sign
  def __digits = @digits
  def __exp = @exp

  # ---- magnitudes: arrays of decimal digits, least significant first ----

  def self.__cmp_mag(a, b)
    return a.length <=> b.length if a.length != b.length
    i = a.length - 1
    while i >= 0
      return a[i] <=> b[i] if a[i] != b[i]
      i -= 1
    end
    0
  end

  def self.__add_mag(a, b)
    r = []
    carry = 0
    i = 0
    n = a.length > b.length ? a.length : b.length
    while i < n
      s = (i < a.length ? a[i] : 0) + (i < b.length ? b[i] : 0) + carry
      r << s % 10
      carry = s / 10
      i += 1
    end
    r << carry if carry > 0
    r
  end

  # a - b for a >= b
  def self.__sub_mag(a, b)
    r = []
    borrow = 0
    i = 0
    while i < a.length
      d = a[i] - (i < b.length ? b[i] : 0) - borrow
      if d < 0
        d += 10
        borrow = 1
      else
        borrow = 0
      end
      r << d
      i += 1
    end
    r.pop while r.length > 0 && r[r.length - 1] == 0
    r
  end

  def self.__mul_mag(a, b)
    return [] if a.empty? || b.empty?
    r = Array.new(a.length + b.length, 0)
    i = 0
    while i < a.length
      carry = 0
      j = 0
      while j < b.length
        t = r[i + j] + a[i] * b[j] + carry
        r[i + j] = t % 10
        carry = t / 10
        j += 1
      end
      k = i + b.length
      while carry > 0
        t = r[k] + carry
        r[k] = t % 10
        carry = t / 10
        k += 1
      end
      i += 1
    end
    r.pop while r.length > 0 && r[r.length - 1] == 0
    r
  end

  # a times 10 ** k
  def self.__shift_mag(a, k)
    return a if a.empty? || k <= 0
    Array.new(k, 0) + a
  end

  # [quotient, remainder] of a / b (b nonzero), by long division
  def self.__divmod_mag(a, b)
    q = Array.new(a.length, 0)
    rem = []
    i = a.length - 1
    while i >= 0
      rem = [a[i]] + rem
      rem.pop while rem.length > 0 && rem[rem.length - 1] == 0
      d = 0
      while __cmp_mag(rem, b) >= 0
        rem = __sub_mag(rem, b)
        d += 1
      end
      q[i] = d
      i -= 1
    end
    q.pop while q.length > 0 && q[q.length - 1] == 0
    [q, rem]
  end

  # ---- conversion ----

  def self.__from(v)
    return v if v.is_a?(BigDecimal)
    return __parse(v.to_s) if v.is_a?(Integer) || v.is_a?(Float)
    raise TypeError, "#{v.class} can't be coerced into BigDecimal"
  end

  def self.__parse(s)
    str = s.strip.delete("_")
    m = /\A([-+]?)(\d*)(?:\.(\d*))?(?:[eE]([-+]?\d+))?\z/.match(str)
    raise ArgumentError, "invalid value for BigDecimal(): \"#{s}\"" unless m
    whole = m[2]
    frac = m[3] || ""
    raise ArgumentError, "invalid value for BigDecimal(): \"#{s}\"" if whole.empty? && frac.empty?
    all = whole + frac
    digits = []
    i = all.length - 1
    while i >= 0
      digits << all[i].to_i
      i -= 1
    end
    exp = (m[4] ? m[4].to_i : 0) - frac.length
    BigDecimal.new(m[1] == "-" ? -1 : 1, digits, exp)
  end

  def coerce(other) = [BigDecimal.__from(other), self]

  # ---- arithmetic ----

  # the signed sum of (s1, a, e) and (s2, b, e2)
  def self.__add(s1, a, e1, s2, b, e2)
    e = e1 < e2 ? e1 : e2
    x = __shift_mag(a, e1 - e)
    y = __shift_mag(b, e2 - e)
    return BigDecimal.new(s2, y, e) if s1 == 0
    return BigDecimal.new(s1, x, e) if s2 == 0
    return BigDecimal.new(s1, __add_mag(x, y), e) if s1 == s2
    c = __cmp_mag(x, y)
    return BigDecimal.new(0, [], 0) if c == 0
    c > 0 ? BigDecimal.new(s1, __sub_mag(x, y), e) : BigDecimal.new(s2, __sub_mag(y, x), e)
  end

  def +(other)
    o = BigDecimal.__from(other)
    BigDecimal.__add(@sign, @digits, @exp, o.__sign, o.__digits, o.__exp)
  end

  def -(other)
    o = BigDecimal.__from(other)
    BigDecimal.__add(@sign, @digits, @exp, -o.__sign, o.__digits, o.__exp)
  end

  def *(other)
    o = BigDecimal.__from(other)
    BigDecimal.new(@sign * o.__sign, BigDecimal.__mul_mag(@digits, o.__digits), @exp + o.__exp)
  end

  def /(other)
    o = BigDecimal.__from(other)
    raise ZeroDivisionError, "divided by 0" if o.__sign == 0
    return BigDecimal.new(0, [], 0) if @sign == 0
    b = o.__digits
    # scale the dividend so the quotient has DIV_DIGITS significant digits
    k = DIV_DIGITS + b.length - @digits.length
    k = 0 if k < 0
    qr = BigDecimal.__divmod_mag(BigDecimal.__shift_mag(@digits, k), b)
    q = qr[0]
    # round half up: the remainder doubled against the divisor
    q = BigDecimal.__add_mag(q, [1]) if BigDecimal.__cmp_mag(BigDecimal.__add_mag(qr[1], qr[1]), b) >= 0
    BigDecimal.new(@sign * o.__sign, q, @exp - o.__exp - k)
  end

  def -@ = BigDecimal.new(-@sign, @digits, @exp)
  def +@ = self
  def abs = BigDecimal.new(@sign == 0 ? 0 : 1, @digits, @exp)
  def zero? = @sign == 0
  def negative? = @sign < 0
  def positive? = @sign > 0

  # ---- rounding ----

  # A mode argument -- a ROUND_* Integer or a Symbol -- as its Integer.
  # The Symbols are CRuby's: :default is the global half up (the only
  # default this package has, since BigDecimal.mode is absent), :truncate
  # and :banker alias :down and :half_even, :ceil aliases :ceiling.
  def self.__round_mode(mode)
    case mode
    when ROUND_UP, :up then ROUND_UP
    when ROUND_DOWN, :down, :truncate then ROUND_DOWN
    when ROUND_HALF_UP, :half_up, :default then ROUND_HALF_UP
    when ROUND_HALF_DOWN, :half_down then ROUND_HALF_DOWN
    when ROUND_CEILING, :ceiling, :ceil then ROUND_CEILING
    when ROUND_FLOOR, :floor then ROUND_FLOOR
    when ROUND_HALF_EVEN, :half_even, :banker then ROUND_HALF_EVEN
    else
      raise ArgumentError, "invalid rounding mode (#{mode})"
    end
  end

  # round(), round(n) and round(n, mode). No argument, or a single n < 1,
  # answers an Integer (CRuby's rule); a positive n, or any explicit mode,
  # answers a BigDecimal. `n` counts decimal places, so a negative n rounds
  # to a power of ten.
  #
  # The digit to drop at the rounding position is `v`, whether any nonzero
  # digit follows it `further`, and the kept magnitude is `q`. initialize
  # strips zeros at both ends of the mantissa, so digits[0] is nonzero; when
  # anything is dropped there is therefore a nonzero digit below the
  # position, and `further` is just "was more than one digit dropped".
  def round(*args)
    raise ArgumentError, "wrong number of arguments (given #{args.length}, expected 0..2)" if args.length > 2
    mode = ROUND_HALF_UP
    if args.length == 0
      n = 0
      to_int = true
    elsif args.length == 1
      n = args[0]
      to_int = n < 1
    else
      n = args[0]
      mode = BigDecimal.__round_mode(args[1])
      to_int = false
    end
    n = n.to_i if n.is_a?(Float)  # CRuby's NUM2INT truncates a Float
    return to_int ? 0 : BigDecimal.new(0, [], 0) if @sign == 0
    # CRuby rounds from a base-1e9 digit array, so when the rounding
    # position falls left of the first stored group -- the value is far
    # below the unit -- every mode but CEILING and FLOOR truncates to zero,
    # and those two answer the unit. `exp10` is CRuby's VpExponent10 (digits
    # before the decimal point) and 9 is its BASE_FIG on a 64-bit build.
    exp10 = @digits.length + @exp
    if n + 9 * ((exp10 + 8) / 9) < 0
      return 0 if to_int
      return BigDecimal.new(1, [1], -n) if mode == ROUND_CEILING && @sign > 0
      return BigDecimal.new(-1, [1], -n) if mode == ROUND_FLOOR && @sign < 0
      raise NotImplementedError, "BigDecimal#round cannot represent a negative zero (CRuby's -0.0)" if @sign < 0
      return BigDecimal.new(0, [], 0)
    end
    # value * 10 ** n is M * 10 ** (exp + n); a nonnegative power is already
    # on a rounding boundary, so nothing drops.
    return to_int ? to_i : self if @exp + n >= 0
    drop = -(@exp + n)
    q = drop < @digits.length ? @digits[drop, @digits.length - drop] : []
    v = drop <= @digits.length ? @digits[drop - 1] : 0
    further = drop >= 2
    inc = false
    case mode
    when ROUND_UP
      inc = true
    when ROUND_DOWN
      inc = false
    when ROUND_HALF_UP
      inc = v >= 5
    when ROUND_HALF_DOWN
      inc = v > 5 || (v == 5 && further)
    when ROUND_CEILING
      inc = @sign > 0
    when ROUND_FLOOR
      inc = @sign < 0
    when ROUND_HALF_EVEN
      if v > 5
        inc = true
      elsif v == 5
        inc = further || (q.length > 0 && q[0] % 2 == 1)
      end
    end
    out = inc ? BigDecimal.__add_mag(q, [1]) : q
    if out.length == 0
      return 0 if to_int
      # CRuby answers -0.0 for a negative that rounds to zero; this
      # representation has no signed zero. Refuse rather than answer +0.0.
      raise NotImplementedError, "BigDecimal#round cannot represent a negative zero (CRuby's -0.0)" if @sign < 0
      return BigDecimal.new(0, [], 0)
    end
    result = BigDecimal.new(@sign, out, -n)
    to_int ? result.to_i : result
  end

  def <=>(other)
    return nil unless other.is_a?(BigDecimal) || other.is_a?(Integer) || other.is_a?(Float)
    (self - BigDecimal.__from(other)).__sign
  end

  def ==(other)
    return false unless other.is_a?(BigDecimal) || other.is_a?(Integer) || other.is_a?(Float)
    (self <=> other) == 0
  end

  def __mantissa_s
    s = +""
    i = @digits.length - 1
    while i >= 0
      s << @digits[i].to_s
      i -= 1
    end
    s
  end

  def to_f
    return 0.0 if @sign == 0
    "#{@sign < 0 ? "-" : ""}#{__mantissa_s}e#{@exp}".to_f
  end

  def to_i
    return 0 if @sign == 0
    m = __mantissa_s
    n = @exp >= 0 ? (m + "0" * @exp).to_i : (m.length + @exp > 0 ? m[0, m.length + @exp].to_i : 0)
    @sign < 0 ? -n : n
  end

  def to_s
    return "0.0" if @sign == 0
    m = __mantissa_s
    "#{@sign < 0 ? "-" : ""}0.#{m}e#{m.length + @exp}"
  end

  def inspect = to_s
end

def BigDecimal(v)
  return BigDecimal.__parse(v) if v.is_a?(String)
  raise ArgumentError, "can't omit precision for a Float." if v.is_a?(Float)
  BigDecimal.__from(v)
end
