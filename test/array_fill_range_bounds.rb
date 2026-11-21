# spinel: int64
# Array#fill(value, range) at the edges of the span, with the Range typed
# and boxed (its ends only known at run time, on a receiver that may not be
# an Array): a begin before the start is a RangeError, the length itself is
# not; an end past the largest Array is an ArgumentError, an inclusive end
# of the largest Integer among them (since CRuby 4.0.10: its end + 1
# wrapped before, and filled nothing).
class Tape
  def fill(*) = :tape
end

def boxed = ARGV.empty? ? [1, 2, 3] : Tape.new
def num(n) = ARGV.empty? ? n : n.to_f

def try
  p yield
rescue RangeError, ArgumentError => e
  puts "#{e.class}: #{e.message}"
end

big = 9_223_372_036_854_775_807

try { [1, 2, 3].fill(0, -4..) }
try { [1, 2, 3].fill(0, -3..) }
try { [1, 2, 3].fill(0, -4..1) }
try { [1, 2, 3].fill(0, -3..1) }
try { [1, 2, 3].fill(0, 0...big) }
try { [1, 2, 3].fill(0, 0..big) }
try { [1, 2, 3].fill(0, 0..(big - 1)) }
try { [1, 2, 3].fill(0, 0..-4) }
try { [1, 2, 3].fill(0, 1..-5) }

try { boxed.fill(0, num(-4)..) }
try { boxed.fill(0, num(-3)..) }
try { boxed.fill(0, num(-4)..num(1)) }
try { boxed.fill(0, num(-3)..num(1)) }
try { boxed.fill(0, num(0)...big) }
try { boxed.fill(0, num(0)..big) }
try { boxed.fill(0, num(0)..(big - 1)) }
try { boxed.fill(0, num(0)..num(-4)) }
try { boxed.fill(0, num(5)..num(6)) }
