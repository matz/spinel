# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
# spinel: decisions
# An index `x + c` or `x - c` read, compared or stored in a loop that holds
# the array's header is summed wrapped, and its checked sum is taken again
# only where the bounds compare fails. An x next to 2**63 - 1 or -2**63
# whose sum leaves the word still raises RangeError (CRuby's, for the Bignum
# index), a sum that stays inside reads, appends, counts from the end or
# raises IndexError as before, and a nil x, or a nil array read through a
# field, raises NoMethodError.

MAX = 9223372036854775807
MIN = -9223372036854775808

def read_plus(a, x)
  v = nil
  k = 0
  while k < a.size
    v = a[x + 1]
    k += 2
  end
  v
end

def read_minus(a, x)
  v = nil
  k = 0
  while k < a.size
    v = a[x - 1]
    k += 2
  end
  v
end

def read_plus_neg(a, x)
  v = nil
  k = 0
  while k < a.size
    v = a[x + -2]
    k += 2
  end
  v
end

def read_minus_neg(a, x)
  v = nil
  k = 0
  while k < a.size
    v = a[x - -2]
    k += 2
  end
  v
end

def float_sum(f, x)
  s = 0.0
  k = 0
  while k < f.size
    s += f[x + 1]
    k += 2
  end
  s
end

def store_plus(a, x, v)
  k = 0
  while k < 1
    a[x + 1] = v
    k += 1
  end
  a
end

def store_minus(a, x, v)
  k = 0
  while k < 1
    a[x - 1] = v
    k += 1
  end
  a
end

def float_store(f, x, v)
  k = 0
  while k < 1
    f[x - -1] = v
    k += 1
  end
  f
end

def nil_operand(a, xs)
  v = nil
  k = 0
  while k < 3
    x = xs[k]
    v = a[x + 1]
    k += 1
  end
  v
end

def count_eq(a, x)
  n = 0
  k = 0
  while k < a.size
    n += 1 if a[x + 1] == 20
    k += 1
  end
  n
end

def float_operand(f, x)
  s = 0.0
  k = 0
  while k < f.size
    s = s + f[x - 1] * 2.0
    k += 2
  end
  s
end

class Holder
  attr_accessor :items

  def initialize
    @items = nil
  end
end

def field_read(h, x)
  v = nil
  k = 0
  while k < 2
    v = h.items[x + 1]
    k += 1
  end
  v
end

def store_maybe_nil(a, x, z)
  k = 0
  while k < 2
    w = k == z ? nil : 7
    a[x + 1] = w
    k += 1
  end
  a
end

def float_store_maybe_nil(f, x, z)
  k = 0
  while k < 2
    w = k == z ? nil : 9.5
    f[x - 1] = w
    k += 1
  end
  f
end

def try(label)
  p [label, yield]
rescue RangeError, IndexError, NoMethodError, TypeError => e
  p [label, e.class]
end

a = [10, 20, 30, 40]
f = [1.5, 2.5, 3.5, 4.5]
[0, 3, -1, MAX, MIN].each { |x| try("read #{x} + 1") { read_plus(a, x) } }
[0, 4, MAX, MIN, MIN + 1].each { |x| try("read #{x} - 1") { read_minus(a, x) } }
[3, 1, MAX, MIN, MIN + 1, MIN + 2].each { |x| try("read #{x} + -2") { read_plus_neg(a, x) } }
[0, 2, MAX, MAX - 1, MAX - 2, MIN].each { |x| try("read #{x} - -2") { read_minus_neg(a, x) } }
[0, 2, 3, MAX, MIN].each { |x| try("float #{x} + 1") { float_sum(f, x) } }
[0, 2, -2, MAX, MIN].each { |x| try("compare #{x} + 1") { count_eq(a, x) } }
[1, 4, MAX, MIN, MIN + 1].each { |x| try("float operand #{x} - 1") { float_operand(f, x) } }
h = Holder.new
try("nil receiver") { field_read(h, 0) }
h.items = a
[0, 3, -5, MAX, MIN].each { |x| try("field #{x} + 1") { field_read(h, x) } }

[0, 2, -2, -5, MAX, MIN].each { |x| try("store #{x} + 1") { store_plus([1, 2, 3], x, 7) } }
[3, 4, 0, -3, MIN, MIN + 1].each { |x| try("store #{x} - 1") { store_minus([1, 2, 3], x, 7) } }
[1, 2, -3, -4, MAX, MIN].each { |x| try("float store #{x} - -1") { float_store([0.5, 1.5], x, 9.5) } }
[0, 2, -4, MAX, MIN].each { |x| try("store maybe nil #{x} + 1") { store_maybe_nil([1, 2, 3], x, 1) } }
[3, 0, MIN, MIN + 1].each { |x| try("float store maybe nil #{x} - 1") { float_store_maybe_nil([0.5, 1.5], x, 0) } }

try("nil operand") { nil_operand(a, [0, 1]) }
