# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
# Under promote, an index `x + c` or `x - c` read, compared or stored in a
# loop that holds the array's header is summed wrapped while x is an Integer
# the word holds, and an x that is not one (a Bignum, nil) takes the
# out-of-range path, which adds as promote does. An x stepping past -2**63
# or 2**63 - 1 into a Bignum, whose sum comes back into the word, reads,
# compares and stores what CRuby does, and a nil x or a nil array raises
# NoMethodError.

def read_down(a, x)
  s = 0
  k = 0
  while k < 3
    v = a[x + 1]
    s = s * 100 + (v.nil? ? 99 : v)
    x -= 1
    k += 1
  end
  s
end

def read_up(a, x)
  s = 0
  k = 0
  while k < 3
    v = a[x - 1]
    s = s * 100 + (v.nil? ? 99 : v)
    x += 1
    k += 1
  end
  s
end

def float_down(f, x)
  s = 0.0
  k = 0
  while k < 2
    s += f[x + -1]
    x -= 1
    k += 1
  end
  s
end

def store_down(a, x, v)
  k = 0
  while k < 2
    x -= 1
    a[x + 1] = v
    k += 1
  end
  a
end

def float_store_down(f, x, v)
  k = 0
  while k < 2
    x -= 1
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
    v = a[x - 1]
    k += 1
  end
  v
end

def compare_up(a, x)
  n = 0
  k = 0
  while k < 3
    n = n * 10 + 1 if a[x - 1] == 20
    n += 2
    x += 1
    k += 1
  end
  n
end

def float_operand_down(f, x)
  s = 0.0
  k = 0
  while k < 2
    s = s + f[x - 1] * 2.0
    x -= 1
    k += 1
  end
  s
end

class Holder
  attr_accessor :items

  def initialize
    @items = nil
  end
end

def field_down(h, x)
  s = 0
  k = 0
  while k < 3
    v = h.items[x + 1]
    s = s * 100 + (v.nil? ? 99 : v)
    x -= 1
    k += 1
  end
  s
end

def store_maybe_nil_down(a, x, z)
  k = 0
  while k < 2
    x -= 1
    w = k == z ? nil : 7
    a[x + 1] = w
    k += 1
  end
  a
end

def try(label)
  p [label, yield]
rescue RangeError, IndexError, NoMethodError, TypeError => e
  p [label, e.class]
end

a = [10, 20, 30, 40]
f = [1.5, 2.5, 3.5, 4.5]
[2, 0, -3, 9223372036854775805, -9223372036854775807].each { |x| try("read #{x} + 1, down") { read_down(a, x) } }
[2, 4, -2, 9223372036854775806, -9223372036854775806].each { |x| try("read #{x} - 1, up") { read_up(a, x) } }
[2, 1, 9223372036854775806, -9223372036854775806].each { |x| try("float #{x} + -1, down") { float_down(f, x) } }
[2, 4, -1, -3, -9223372036854775808].each { |x| try("store #{x} - 1 + 1, down") { store_down([1, 2, 3], x, 7) } }
[1, 2, -1, -9223372036854775808].each { |x| try("float store #{x} - 1 - -1, down") { float_store_down([0.5, 1.5], x, 9.5) } }
[1, 2, 9223372036854775806].each { |x| try("compare #{x} - 1, up") { compare_up(a, x) } }
[2, 3, -9223372036854775807].each { |x| try("float operand #{x} - 1, down") { float_operand_down(f, x) } }
h = Holder.new
try("nil receiver") { field_down(h, 0) }
h.items = a
[2, 0, -9223372036854775807].each { |x| try("field #{x} + 1, down") { field_down(h, x) } }
[2, 4, -9223372036854775808].each { |x| try("store maybe nil #{x} - 1 + 1, down") { store_maybe_nil_down([1, 2, 3], x, 1) } }
try("nil operand") { nil_operand(a, [1, 2]) }
