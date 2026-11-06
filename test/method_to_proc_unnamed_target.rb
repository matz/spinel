# A Method the analysis cannot name -- kept in an instance variable, a
# constant, a global or a Hash, or a local written to two methods -- called
# through its to_proc or handed over as a block: those calls type the
# target's parameters, whatever the arguments are.
class Show
  def initialize(flag) = @m = flag ? method(:one) : method(:two)
  def one(x) = "one:#{x.inspect}"
  def two(a, b) = "two:#{a.inspect},#{b.inspect}"
  def show1(v) = @m.to_proc.call(v)
  def show2(a, b) = @m.to_proc.call(a, b)
end
s = Show.new(true)
p s.show1(1.5)
p s.show1([1, 2])
p s.show1(nil)
p s.show1(Show)
p s.show1("str")
p Show.new(false).show2({k: 1}, :sym)

def len(s) = s.length
LEN = method(:len)
$len = method(:len)
TABLE = {len: method(:len)}
p LEN.to_proc.call("abcd")
p $len.to_proc.call("ab")
p TABLE[:len].to_proc.call("abc")

class Many
  def a(x) = "a:#{x}"
  def b(x) = "b:#{x}"

  def twice
    yield "p"
    yield "q"
  end

  def run(flag)
    m = method(:a)
    m = method(:b) if flag
    p twice(&m)
    one = method(:a)
    p twice(&one)
  end
end
Many.new.run(true)
Many.new.run(false)

# A local written to two Methods, one of them also called directly with an
# Integer: each yield of the block binds both targets.
class Both
  def a(x) = "a:#{x.inspect}"
  def b(x) = "b:#{x.inspect}"

  def twice
    yield "p"
    yield "q"
  end

  def run(flag)
    m = method(:a)
    m = method(:b) if flag
    p twice(&m)
  end
end
Both.new.run(true)
Both.new.run(false)
p Both.new.a(1)

# Proc#yield as the only call of the to_proc.
def slashed(k) = k.include?("/")
SLASHED = method(:slashed)
p SLASHED.to_proc.yield("a/b")
p SLASHED.to_proc.yield("ab")

# A Method handed as a block to a method that hands its &blk on to one
# that yields.
class Fwd
  def initialize = @rd = method(:dotted)
  def dotted(k) = k.include?(".")

  def twice
    yield "a"
    yield "b.c"
  end

  def fwd(&blk) = twice(&blk)
  def run = p(fwd(&@rd))
end
Fwd.new.run
