# chunk_while / slice_when over an empty Array literal: the runs are an
# Array's, as over a typed Array, so the Array `map(&:size)` builds over them
# answers partition, take_while and drop_while (the literal has no element
# type of its own, and the call was typed an Enumerator the routes had no
# arm for).
a = [].chunk_while { |x, y| y == x + 1 }.map(&:size)
p a.partition(&:even?)
p a.take_while { |e| e < 3 }
p a.drop_while { |e| e < 3 }
def f(n)
  [].chunk_while { |x, y| y == x + 1 }.map(&:size).take_while { |e| e < n }
end
p f(3)
b = [].slice_when { |x, y| y != x + 1 }.map(&:size)
p b.partition(&:even?)
c = [1, 2, 4].chunk_while { |x, y| y == x + 1 }.map(&:size)
p c.partition(&:even?)
