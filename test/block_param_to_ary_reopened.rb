# A block of several parameters takes a yielded object apart through the
# to_ary that runs when the block runs, as a for loop's targets, a nested
# parameter and the numbered parameters do: a method called before and
# after a reopen takes the version defined by then. An Array, a String
# and nil are not converted; a to_ary that answers nil leaves the object
# whole, and one that answers anything else but an Array is a TypeError.

class K
  def to_ary = [1, 2]
  def inspect = "#<K>"
end

def pairs(xs) = xs.map { |a, b| [a, b] }
def nested(xs) = xs.map { |(a, b), c| [a, b, c] }
def numbered(xs) = xs.map { [_1, _2] }

def looped(xs)
  r = []
  for a, b in xs
    r << [a, b]
  end
  r
end

def run(pr, o) = pr.call(o)

xs = [K.new, [7, 8], "s", nil]
pr = proc { |a, b, k: 0| [a, b, k] }
p pairs(xs), nested([[K.new, 9]]), numbered(xs), looped(xs), run(pr, K.new)

class K
  def to_ary = [3, 4]
end
p pairs(xs), nested([[K.new, 9]]), numbered(xs), looped(xs), run(pr, K.new)

class K
  def to_ary = nil
end
p pairs(xs), nested([[K.new, 9]]), looped(xs)

class K
  def to_ary = 42
end
[-> { pairs(xs) }, -> { nested([[K.new, 9]]) }, -> { looped(xs) }].each do |f|
  p f.call
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
