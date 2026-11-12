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
