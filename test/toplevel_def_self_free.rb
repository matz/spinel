# A top-level def whose body does not read self is Object's public method once
# `public` opens it, so an explicit receiver reaches it, through helper defs
# that do not read self either.
def leaf(x) = x + 1
def mid(x) = leaf(x) * 2
def top(x, k: 3) = mid(x) + k
def fmt(x) = format("%03d", x)
def pick(a, b = a + 1) = [a, b]
def puts_it(x) = $stdout.write("v=#{x}\n")
def rec(n) = n <= 0 ? 0 : n + rec(n - 1)

def redefined(x) = x
def redefined(x) = x * 10
def uses_redefined(x) = redefined(x)

public :leaf, :mid, :top, :fmt, :pick, :puts_it, :rec, :uses_redefined

p 5.leaf(1)
p "s".mid(2)
p [1].top(3)
p :a.top(3, k: 7)
p nil.fmt(7)
p 1.pick(4)
p 2.0.pick(4, 9)
"x".puts_it(3)
p 4.rec(4)
p Object.new.uses_redefined(2)
p 1.respond_to?(:mid)
