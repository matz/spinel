# A builtin's Method called with a block's parameter (#10719): the
# parameter may be nil, so the argument is checked where the builtin reads
# it -- the coercion TypeError for an operator's operand, a TypeError for
# another argument -- and the program compiles.
m = 10.method(:+)
pr = proc { |x| m.call(x) }
p pr.call(2)
begin
  pr.call
rescue TypeError => e
  p e.message
end
f = 1.5.method(:*)
fp = proc { |x| f.call(x) }
p fp.call(2.0)
g = 12.method(:gcd)
q = proc { |x| g.call(x) }
p q.call(8)
begin
  q.call
rescue TypeError => e
  p e.class
end
p [1, 2].map { |x| m.call(x) }
