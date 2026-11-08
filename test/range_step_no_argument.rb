# spinel: int64
# A Range#step with no argument steps by 1, as step(1) does: the Integer, Float
# and String ranges, blockless (the stepped values, an endless range walked as
# it is read) and with a literal block (the call answers the range), through a
# parameter, a nil block argument and the safe-navigation form, and a literal
# block on a boxed receiver beside a program class's own step. The call stays
# as written: a String range's Enumerator inspects as `step`, not `step(1)`.
# A step given an argument, % and a program class's own step are unchanged.
def t
  r = yield
  puts r.inspect
rescue Exception => e
  puts "ERR #{e.class}: #{e.message}"
end

t { (1..6).step.to_a }
t { (1...6).step.to_a }
t { (6..1).step.to_a }
t { (3...3).step.to_a }
t { (-3..3).step.to_a }
t { (1..6).step.size }
t { (1..6).step.first(2) }
t { (1..6).step.map { |x| x * 2 } }
t { (1..6).step.sum }
t { (1..6).step.include?(3) }
t { x = (1..6).step; x.to_a + x.to_a }
t { (1..6).step.lazy.map { |x| x + 1 }.first(2) }

t { (1..).step.first(3) }
t { (1..).step.lazy.map { |x| x * 2 }.first(3) }
t { r = []; (1..).step { |x| r << x; break if x > 3 }; r }

t { (1.0..3.0).step.to_a }
t { (1.0...3.0).step.to_a }
t { (1.5..4).step.to_a }
t { (1..2.5).step.to_a }
t { (1.0..3.0).step.size }
t { (1.5..).step.first(2) }
t { (3.0..1.0).step.to_a }

t { ("a".."e").step.to_a }
t { ("a"..."e").step.to_a }
t { ("a"..).step.first(3) }
t { ("az".."bc").step.to_a }
t { ("a".."e").step.map(&:upcase) }
p(("a".."e").step)
p(("a".."e").step(1))
p(("a".."e").step.size)

t { r = []; (1..6).step { |x| r << x }; r }
t { r = []; (1..6).step { |x| r << x }.class }
t { (1..6).step { |x| x } }
t { r = []; (6..1).step { |x| r << x }; r }
t { r = []; (1..6).step { |x| next if x.odd?; r << x }; r }
t { (1..6).step { |x| break x * 10 if x == 3 } }
t { r = []; (1..6).step { r << _1 }; r }
t { r = []; (1..6).step { r << it }; r }
t { r = []; (1.0..3.0).step { |x| r << x }; r }
t { r = []; ("a".."e").step { |x| r << x }; r }
t { s = 0; (1..).step { |x| s += x; break if x == 4 }; s }

t { ((2**40)..(2**40 + 3)).step.to_a }
t { ((2**40)...(2**40 + 3)).step.size }
t { (2**40..).step.first(2) }

def go(r) = r.step.to_a
t { go(1..4) }
t { go(2..5) }
def fr = (1.5..3.5)
t { fr.step.to_a }

r = (1..6)
p r.step(&nil).to_a
p r&.step&.to_a
p r&.step&.size
p (1.0..2.0).step(&nil).to_a
p ("a".."c").step(&nil).to_a
n = nil
p n&.step
p n&.step&.to_a

t { (1..6).step(2).to_a }
t { (1..6).step(1).to_a }
t { ((1..6) % 2).to_a }
t { (1..6).%.to_a }
t { (1..6).step(0) }
t { (1..6).step(-1).to_a }
t { (6..1).step(-2).to_a }
t { (1..6).step(2, 3) }
t { ("a".."e").step(2).to_a }
t { (1.0..3.0).step(0.5).to_a }

class Box
  def step(*a) = [:box, a]
end
t { Box.new.step }
t { Box.new.step(2) }
def mk(f) = f ? (1..4) : Box.new
t { mk(false).step }
t { mk(true).step(2).to_a }
def mixed(f) = f ? (1..3) : (1.0..2.0)
t { acc = []; mixed(true).step { |x| acc << x }; acc }
t { acc = []; mixed(false).step { |x| acc << x }; acc }
t { acc = []; mk(true).step { |x| acc << x }; acc }

# a block that takes no value counts the walk of a range with a Float end
p((1..2.5).step { })
t { n = 0; (1..2.5).step { n += 1 }; n }
t { n = 0; (1..2.5).step(1) { n += 1 }; n }
t { n = 0; (1..2.5).step { n += _1 }; n }

# a block with an optional, post, keyword or destructuring parameter, or
# block-local variables, takes the walk's values as step(1)'s does
t { a = []; (1..2).step { |v = 0| a << v }; a }
t { a = []; (1..2).step { |*, z| a << z }; a }
t { a = []; (1..2).step { |k: 0| a << k }; a }
t { a = []; (1..2.5).step { |v = 0| a << v }; a }
t { a = []; (1..2).step { |;y| y = 1; a << y }; a }
t { a = []; (1..2).step { |(u, w)| a << u }; a }
t { a = []; (1..2).step { |k, | a << k }; a }
t { a = []; (1..2.5).step { |k: 0| a << k }; a }
