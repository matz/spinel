# Array#rfind (Ruby 4.0) on a receiver that is an Integer array at one call
# site and a String array at another. The receiver is only known at run time,
# so the call used to compile and then die with NoMethodError.
def last_not_three(a)
  p a.rfind { |x| x != 3 }
end
last_not_three([3, 1, 2])
last_not_three(["b", "a"])
last_not_three([3, 3])
last_not_three([])

# the block sees the elements back to front, and a hit stops the walk
def walk(a)
  seen = []
  r = a.rfind { |x| seen << x; x == 1 || x == "a" }
  p [r, seen]
end
walk([1, 2, 3])
walk(["a", "b", "c"])

# typed receivers keep the path they had
p [3, 1, 2].rfind { |x| x != 3 }
p %w[b a].rfind { |x| x != "z" }

# the same through a yield whose blocks give different element types: the
# lowering used to be committed while the yielded Array was still an Integer
# array, and the later widening left a find loop shaped for Integers
def yielded_last_not_three
  p yield.rfind { |x| x != 3 }
end
yielded_last_not_three { [3, 1, 2] }
yielded_last_not_three { ["b", "a"] }
yielded_last_not_three { [3, 3] }

# a class of the program that defines its own rfind, held in the same
# variable as an Array: the Array is answered as an Array and the instance by
# its own method
class Shelf
  def rfind
    yield 7
    "shelf"
  end
end
def either(a)
  p a.rfind { |x| x != 3 }
end
either([3, 1, 2])
either(Shelf.new)
either(["b", "a"])

# only an Array answers rfind: a Hash, a Range and an Enumerable class raise
# NoMethodError, typed or held in a poly variable
class Bag
  include Enumerable
  def each
    yield 1
    yield 2
  end
end
def raises(label)
  r = begin
    yield
  rescue NoMethodError
    "NoMethodError"
  end
  puts "#{label}: #{r.inspect}"
end
def ask(o)
  o.rfind { |x| x }
end
raises("hash") { { a: 1, b: 2 }.rfind { |k, v| v > 0 } }
raises("range") { (1..3).rfind { |x| x > 0 } }
raises("bag") { Bag.new.rfind { |x| x > 0 } }
raises("poly array") { ask([1, 2]) }
raises("poly bag") { ask(Bag.new) }

# `rfind.each { }` is `rfind { }` (the Enumerator's each runs the method it
# came from): the typed lowering turns the blockless inner call into
# reverse.find, and the chained call must be answered by a copy of find, not
# of rfind, or the array is reversed twice and the first match comes back.
p [1, 2, 3].rfind.each { |x| x < 3 }
p [1, 2, 3].rfind.each { |x| false }
p %w[a bb cc].rfind.each { |s| s.size == 2 }
