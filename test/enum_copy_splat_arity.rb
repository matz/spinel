# A splat call of an Enumerable method written in Ruby (a `module
# Enumerable` reopening's, or builtins/'s) reaches its definition, and its
# count is judged at run time as CRuby judges it, the receiver left out

module Enumerable
  def pick(i, j = 0) = to_a[i + j]
  def grab(i, *more) = [to_a[i], more]
  def opt2(a = 1, b = 2) = [count, a, b]
  def req2(a, b) = [count, a, b]
  def none2? = count == 0
end

def try
  p yield
rescue ArgumentError => e
  p e.message
end

z = [0, 0, 0]
try { [5].pick(*z.take(0)) }
try { [5].pick(*z.take(1)) }
try { [5, 6].pick(*[1, 0, 0].take(2)) }
try { [5].pick(*z) }

# a nil splat is no argument
n = [nil, [1]][0]
try { [5].pick(*n) }
p [].none2?(*n)

# optional and rest parameters
e = [9, 9, 9, 9]
try { [4, 5].grab(*e.take(0)) }
try { [4, 5].grab(*[1, 2, 3].take(1)) }
try { [4, 5].grab(*[1, 2, 3].take(3)) }
try { [4].opt2(*e.take(0)) }
try { [4].opt2(*e.take(2)) }
try { [4].opt2(*e.take(3)) }
try { [4].req2(*e.take(1)) }
try { [4].req2(1, *e.take(1)) }
try { [4].req2(1, *e.take(2)) }

# two calls in one statement
a = [0]
b = [0, 1]
p [[5, 6].pick(*a), [5, 6].pick(*b)]
try { [[5].pick(*a), [5].pick(*a.take(0))] }

# boxed receivers
[[5, 6], (7..9), {a: 1}].each do |x|
  try { x.pick(*[0, 1].take(1)) }
  try { x.pick(*z) }
end

# through rest parameters, and in tail position
def l3(x, *r) = x.pick(*r)
def l2(x, *r) = l3(x, *r)
def l1(x, *r) = l2(x, *r)
p l1([5, 6, 7], 1), l1([5, 6, 7], 1, 1)
try { l1([5]) }
def t(x, r)
  return x.pick(*r) if r.size > 1
  x.pick(*r)
end
p t([4, 5], [1]), t([4, 5, 6], [1, 1])

# builtin methods, and builtins/' tally
r = [1, 2]
p [3, 1, 2].min(*r.take(1)), [3, 1, 2].first(*r.take(1))
try { [1, 1, 2].tally(*r) }
