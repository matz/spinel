# A local reads nil until a write runs. An Integer or Float local whose
# writes ahead of a read are all conditional -- a modifier `if`, one branch
# of an `if`, a loop body, a multiple assignment under `if false`, a body a
# rescue left early -- started at 0 or 0.0, and the read answered that.
def t
  nl = nil
  mk, x = 0, nl if false
  p mk
  v = 1 if false
  p v, v.nil?, v.to_s
  w = 2.5 if false
  p w
  i = 0
  while i < 2
    k = i * 10
    i += 1
  end
  p k
  j = 5
  while j < 3
    n = j
    j += 1
  end
  p n
  begin
    u = Integer("x")
  rescue ArgumentError
  end
  p u
end
t

def both(c)
  if c
    x = 1
  else
    x = 2
  end
  case c
  when true then y = 3
  else y = 4
  end
  q = (r = 5) + 1
  p [x, y, r, q]
end
both(true)
both(false)

f = 1.5 if ARGV.size > 3
p f
a, b = 1, 2 if ARGV.empty?
p a, b

# `&&=` writes only a local already truthy, so an unassigned one stays nil;
# `+=` on one raises, as nil + 1 does
def and_write(c)
  x = 5 if c
  x &&= 7
  g = 2.5 if c
  g &&= 1.5
  p [x, g]
end
and_write(true)
and_write(false)
def op_write(c)
  y = 1 if c
  y += 1
  p y
rescue NoMethodError => e
  p e.message
end
op_write(true)
op_write(false)
