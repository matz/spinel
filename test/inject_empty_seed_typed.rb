# reduce / inject with an empty `[]` seed whose block only pushes Integers
# (or Floats) into the accumulator, reads it, and answers it folds into a
# typed Array, as an each_with_object([]) memo does; any other block keeps
# the boxed fold. infer-test checks that evens, odd_halves, squares, kept,
# nested and small answer an sp_IntArray or an sp_FloatArray.
def evens(n) = (0...n).inject([]) { |acc, i| i.even? ? acc << i : acc }
def odd_halves(n) = (0...n).inject([]) { |acc, i| acc << i * 0.5 if i.odd?; acc }
def squares(n) = (0...n).reduce([]) { |acc, i| acc.push(i * i); acc }
def kept(a) = a.inject([]) { |acc, x| acc << x unless acc.include?(x); acc }
def nested(n) = (0...n).inject([]) { |acc, i| [i, -i].each { |j| acc << j }; acc }
def small(n) = (0...n).inject([]) { |acc, i| if acc.size < 3 then acc << i else acc end }

p evens(9), evens(9).sum
p odd_halves(6), odd_halves(6).sum
p squares(5), squares(0)
p kept([3, 1, 3, 2, 1]), kept([])
p nested(3)
p small(10)
s = 0
100.times { |k| s += (0...50).inject([]) { |acc, i| acc << i * k if i.odd?; acc }.sum }
p s

# a receiver that mixes nil with its Integers, a mixed kind of push, and a
# block that hands the accumulator on or leaves early keep the boxed fold,
# as before (a nil an Integer read answers, `acc << a[i]` past the end, is
# kept in the Integer Array)
p [1, nil, 3].inject([]) { |acc, x| acc << x; acc }
p (0...4).inject([]) { |acc, i| acc << i; acc << "s" if i == 2; acc }
def keep(x) = x
p (0...3).inject([]) { |acc, i| keep(acc); acc << i; acc }
p (0...4).inject([]) { |acc, i| next acc if i == 1; acc << i; acc }
