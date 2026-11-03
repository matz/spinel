# Range.new with a splatted argument list: the array's length picks the
# two-argument or the exclude_end form at run time. Not covered yet: two
# splats (`Range.new(*a, *b)`), a block, and `...` forwarding.
a = 7
b = 3
p Range.new(b, a).to_a
p Range.new(*[a, b].minmax).to_a
lo = [a, b]
p Range.new(*lo).to_a
p Range.new(*lo, true).to_a

def bounds(n) = [1, n]
r = Range.new(*bounds(4))
p r
p r.exclude_end?
p Range.new(*bounds(4), true)
p Range.new(*[2, 5, true])
p Range.new(*[2, 5, false]).to_a

hl = [b, a]
hl << 0 if ARGV.size > 5
p Range.new(*hl).sum

words = ["a", "e"]
p Range.new(*words).to_a

begin
  one = [1]
  one << 2 if ARGV.size > 5
  Range.new(*one)
rescue ArgumentError => e
  p e.message
end

# a computed exclude_end, without a splat
flag = ARGV.empty?
p Range.new(1, 4, flag).to_a
p Range.new(1, 4, !flag).to_a
p Range.new(b, a, ARGV.size > 5)

def make_range(*args) = Range.new(*args)
p make_range(2, 6)
p make_range(2, 6, true).to_a

# a computed flag with computed endpoints: the endpoints are evaluated
# first, in order, and the flag after them
n = 5
p Range.new(0, n - 1, flag).to_a
$log = []
def ev(x) = ($log << x; x)
p Range.new(ev(1), ev(4), ev(true))
p $log
x = 1
p Range.new(x, 4, (x = 2; true))
p x
class Lo
  def initialize = (@lo = 1)
  def bump = (@lo = 3; true)
  def r = Range.new(@lo, 5, bump)
end
p Lo.new.r
s = +"a"
r = Range.new(s, "c", (s << "b"; true))
p r
p r.first.equal?(s)
p [true, false].map { |f| Range.new(f ? 1 : 2, 5, f) }

# a splat followed by a computed flag
p Range.new(*lo, flag)
p Range.new(*lo, ARGV.size > 5)
p Range.new(*hl, flag)

# a `**` that comes out empty passes no flag
none = {}
some = { a: 1 }
p Range.new(1, 2, **none)
p Range.new(1, 2, **some)
p Range.new(*[1, 2], **none)

# any other count raises, after its arguments are evaluated
begin
  Range.new(*[1])
rescue ArgumentError => e
  p e.message
end
begin
  Range.new(ev(1), ev(2), ev(3), ev(4))
rescue ArgumentError => e
  p e.message
end
begin
  Range.new
rescue ArgumentError => e
  p e.message
end
p $log
