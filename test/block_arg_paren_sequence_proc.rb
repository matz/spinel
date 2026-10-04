# A block argument written as a parenthesized sequence ending in a proc
# literal, `&(stmt; proc { ... })`: the proc is the block, after the
# statements run. Nothing followed the literal through the parentheses, so
# its parameters were bound by nothing and read the elements as Integers:
# `{a: 1}.each_with_object([], &(1; proc { |(k, v), m| m << k }))` gave [0].

h = {a: 1}
p h.each_with_object([], &(1; proc { |(k, v), m| m << k }))
p h.each_with_object([], &(nil; proc { |(k, v), m| m << v }))
p h.each_with_object([], &(1; proc { |kv, m| m << kv }))
p h.map(&(1; proc { |k, v| k }))
p [[1, 2]].each_with_object([], &(1; proc { |(x, y), m| m << y }))
p [1, 2].map(&(1; proc { |x| x * 2 }))
p [1, 2].map(&(1; ->(x) { x + 1 }))

# the statements run once, after the receiver and the arguments
$l = []
def lg(x) = ($l << x; x)
p({b: 2}.each_with_object([], &(lg(:seq); proc { |(k, v), m| m << k })))
p lg({c: 3}).each_with_object(lg([]), &(lg(:blk); proc { |(k, v), m| m << v }))
p $l

# Reads precede assignments in the block expression, at the call site.
a = [1]
p a.map(&(a = [2]; proc { |x| x }))
memo = [10]
p [1].each_with_object(memo, &(memo = [20]; proc { |x, m| m << x }))
flag = false
p(flag && [1].map(&(raise "boom"; proc { |x| x })))
p(true || [1].map(&(raise "boom"; proc { |x| x })))
p(false ? [1].map(&(raise "boom"; proc { |x| x })) : :skip)
p [1].map(&(1; proc { |x| x + 2 }))

flag = true
p(flag && [1].map(&(puts :taken; proc { |x| x })))
2.times { p [1].map(&(puts :again; proc { |x| x })) }
def pair(a,b)
  p a, b
end
a = [1]
pair(a, [2].map(&(a = [9]; proc { |x| x })))
