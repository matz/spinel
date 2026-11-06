# A Range literal with a Bignum end, asked about one value: the ends and the
# value are compared in place, as Range#cover? compares them, since no
# sp_Range can hold the end. (addressable's punycode: `(n...m) === x` with
# m starting at 1 << 64.)
big = 1 << 64
neg = -(1 << 70)
p((128...big) === 252)
p((128...big) === 127)
p((128...big) === big)
p((128..big) === big)
p((128..big).include?(big - 1))
p((128..big).member?(big + 1))
p((128..big).cover?(big / 2))
p((neg..0) === -5)
p((neg..0) === 1)
p((neg..) === 10**30)
p((..big) === -3)
p((...big) === big)
p((0...big) === 2.5)
p((0...big) === 1.8e19)
p((0...big) === 1.9e19)
v = ["a", nil, 7, big * 2].map { |x| (0...big) === x }
p v
order = []
r = ((order << :lo; 1)...(order << :hi; big)) === (order << :x; 5)
p r, order

def pick(input, n)
  m = 1 << 64
  input.each { |x| m = x if (n...m) === x }
  m
end
p pick([300, 200, 900, 150], 128)
p pick([1, 2], 128)

# The end is a box that holds the Bignum, read from a constant.
MAXINT = 1 << 64
def lowest_above(input, n)
  m = MAXINT
  input.size.times { |j| m = input[j] if (n...m) === input[j] }
  m
end
p lowest_above("bücher".unpack("U*"), 128)
p lowest_above([1, 2], 128)

# Boxed String ends: === and cover? compare, include? still walks.
lo = "a"
lo = 1 if ARGV.size > 5
hi = "z"
hi = 2 if ARGV.size > 5
p [(lo..hi) === "bb", (lo..hi).cover?("bb"), (lo..hi).include?("bb"), (lo..hi) === 3]
# A Range is no value a numeric Range holds, under ===.
a = 1
a = "x" if ARGV.size > 5
r2 = 2..3
r2 = 9 if ARGV.size > 5
p [(a..MAXINT) === r2, (a..MAXINT) === 5]

# A Complex compares when it is real and answers false when it is not.
zs = [Complex(1, 1), Complex(1, 0), 3]
zs.each { |z| p [(..MAXINT) === z, (0..MAXINT) === z] }

# Ends that do not compare are CRuby's bad value for range, raised before
# the value is evaluated.
bad = "z"
bad = 1 if ARGV.size > 5
seen = []
begin
  p((0..bad) === (seen << :x; 5))
rescue ArgumentError => e
  p [e.message, seen]
end
p((nil..bad) === "c")

# Both ends boxed, the begin too: the Range is never built ahead of the test.
def lowest_from(input, n)
  m = MAXINT
  input.each { |x| m = x if (n...m) === x }
  m
end
start = 128
start = :a if ARGV.size > 5
p lowest_from([300, 200, 900], start)

# A === of the program's own on another class leaves Range#=== as it is, so
# the literal still compares in place.
class Matcher
  def ===(other)
    other == :match
  end
end
p Matcher.new === :match
p lowest_from([300, 250], start)
