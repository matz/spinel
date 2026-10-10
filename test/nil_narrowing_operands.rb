# Nil narrowing after an operator: once `x < y`, `x + y` and the like on an
# Integer or Float have returned, neither operand was nil -- a nil receiver
# is a NoMethodError, a nil argument the number's ArgumentError or
# TypeError -- so a later read of the local needs no nil test, as after
# `x op= v`. Each shape has a twin where the read must keep it: `==` and
# `&.` answer for a nil, and a rescued raise, an arm that did not run or
# an argument that wrote the receiver's local goes on with it nil.
def try
  yield
rescue NoMethodError, ArgumentError, TypeError => e
  puts "#{e.class}: #{e.message}"
end

def low(a, i, lim)            # the receiver
  x = a[i]
  return -1 if x < lim
  s = x > 4
  [x * 2, s]
end

def high(a, i, lim)           # the argument
  y = a[i]
  return -1 if lim > y
  s = y < 9
  [y - lim, s]
end

def sum_pos(a, n)             # in a loop
  s = 0.0
  i = 0
  while i < n
    v = a[i]
    s += v if v > 0.0
    i += 1
  end
  s
end

def arith(a, i)               # arithmetic
  x = a[i]
  y = x + 1
  s = x < 9
  [y, s]
end

def equal(a, i)               # == answers false for a nil
  x = a[i]
  r = x == 5
  s = x < 10
  [r, s]
end

def safe(a, i)                # &. answers nil for a nil receiver
  x = a[i]
  r = x&.<(5)
  s = x < 10
  [r, s]
end

def rescued(a, i)             # a rescued raise
  x = a[i]
  begin
    x < 5
  rescue NoMethodError
  end
  x < 10
end

def reassign(a, i)            # the argument writes the receiver's local
  x = a[i]
  y = x + (x = nil; 1)
  s = x < 10
  [y, s]
end

def cond(a, i, b)             # only the arm that compared
  x = a[i]
  t = b && x < 5
  s = x < 10
  [t, s]
end

ints = [3, 7]
fl = [1.5, -2.0, 4.0]
p low(ints, 1, 5), low(ints, 0, 5)
try { p low(ints, 2, 5) }
p high(ints, 1, 5), high(ints, 0, 5)
try { p high(ints, 2, 5) }
p sum_pos(fl, 3)
try { p sum_pos(fl, 4) }
p arith(ints, 0)
try { p arith(ints, 3) }
p equal(ints, 0)
try { p equal(ints, 9) }
p safe(ints, 0)
try { p safe(ints, 9) }
p rescued(ints, 0)
try { p rescued(ints, 9) }
try { p reassign(ints, 0) }
p cond(ints, 0, true), cond(ints, 1, false)
try { p cond(ints, 9, false) }
try { p cond(ints, 9, true) }
