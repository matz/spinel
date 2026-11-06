# A local a `while` multiplies keeps the kind its writes give it. The shape of
# the loop seeded it as a Bignum, which only an Integer can grow into, and any
# other kind met the seed as untyped: the right answer, on the boxed slow path.
# infer-test holds this file to no --warn-widen warning.
class Vec
  def initialize(v) = @v = v
  def *(k) = Vec.new(@v * k)
  def inspect = "Vec(#{@v})"
end

def half = 2.5

def float_literal(n)
  y = 1.0
  i = 0
  while i < n
    y = y * 10.0
    i += 1
  end
  3.0 / y
end

def float_op_write(n)
  y = 1.0
  i = 0
  while i < n
    y *= 10.0
    i += 1
  end
  y
end

def float_by_integer(n)
  y = 2.0
  i = 0
  while i < n
    y = y * n
    i += 1
  end
  y
end

def float_pow(n)
  y = 2.0
  i = 0
  while i < n
    y = y ** 2
    i += 1
  end
  y
end

def float_from_to_f(n)
  y = n.to_f
  i = 0
  while i < n
    y = y * 2.0
    i += 1
  end
  y
end

def float_from_method(n)
  y = half
  i = 0
  while i < n
    y = y * 2
    i += 1
  end
  y
end

def float_from_parameter(x, n)
  y = x
  i = 0
  while i < n
    y = y * 2
    i += 1
  end
  y
end

def float_fibonacci(n)
  a = 1.0
  b = 1.0
  i = 0
  while i < n
    t = a + b
    a = b
    b = t
    i += 1
  end
  b
end

def float_through_temp(n)
  y = 1.0
  i = 0
  while i < n
    t = y * 1.5
    y = t
    i += 1
  end
  y
end

def string_repeated(n)
  y = "a"
  i = 0
  while i < n
    y = y * 2
    i += 1
  end
  y
end

def array_repeated(n)
  y = [1]
  i = 0
  while i < n
    y = y * 2
    i += 1
  end
  y
end

def rational_grows(n)
  y = 3r
  i = 0
  while i < n
    y = y * 2
    i += 1
  end
  y
end

def object_grows(n)
  y = Vec.new(2)
  i = 0
  while i < n
    y = y * 3
    i += 1
  end
  y
end

# An Integer still grows past the word.
def integer_grows(n)
  y = 2
  i = 0
  while i < n
    y = y * y
    i += 1
  end
  y
end

def integer_small(n)
  y = 1
  i = 0
  while i < n
    y = y * 10
    i += 1
  end
  y
end

p float_literal(3)
p float_op_write(3)
p float_by_integer(3)
p float_pow(3)
p float_from_to_f(3)
p float_from_method(3)
p float_from_parameter(1.5, 3)
p float_fibonacci(3)
p float_through_temp(3)
p string_repeated(3)
p array_repeated(3)
p rational_grows(3)
p object_grows(3)
p integer_grows(7)
p integer_small(3)
