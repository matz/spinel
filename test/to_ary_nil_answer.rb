# A to_ary that answers nil for some receivers and an Array for others keeps
# the receiver when it answers nil, for a program's class and for String, in a
# multiple assignment and in a block of several parameters. The Array it
# answers for the others is not of one element type, so the conversion is not
# typed.
class Pair
  def initialize(skip) = @skip = skip
  def to_ary
    return nil if @skip
    [self, 2]
  end
end
class String
  def to_ary
    return nil if empty?
    [self, 2]
  end
end

a, b = Pair.new(true)
p [a.class, b]
a, b = Pair.new(false)
p [a.class, b]
[Pair.new(true), Pair.new(false)].each { |x, y| p [x.class, y] }
def yl(v) = yield(v)
yl(Pair.new(true)) { |x, y| p [x.class, y] }

a, b = +""
p [a, b]
a, b = +"ab"
p [a, b]
["", "ab"].each { |x, y| p [x, y] }
yl(+"") { |x, y| p [x, y] }
yl(+"cd") { |x, y| p [x, y] }
p(["", "ab"].map { |x, y| y })

# a receiver that runs code runs once, and its nil answer keeps what it built
$n = 0
def mk(skip = true) = ($n += 1; Pair.new(skip))
a, b = mk
p [a.class, b, $n]
a, b = mk(false)
p [a.class, b, $n]
x = (a, b = mk)
p [x.class, a.class, b, $n]
a, *r = mk
p [a.class, r, $n]
yl(mk) { |x, y| p [x.class, y, $n] }
def boom = raise("boom")
begin
  a, b = boom
rescue => e
  p [e.message, $n]
end
