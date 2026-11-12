# rfind on a receiver known only at run time reads the array live at each
# step, as a typed array's loop does: an element the block writes before the
# walk reaches it is the element the walk sees, and an array the block shrinks
# or grows is walked from its current end. (The walk once ran over a reversed
# copy taken before the block, which saw none of it.)
def write_ahead(a)
  a.rfind do |x|
    a[0] = (x.is_a?(Integer) ? 9 : "z") if x == 3 || x == "c"
    x == 9 || x == "z"
  end
end
p write_ahead([1, 2, 3])
p write_ahead(["a", "b", "c"])
p write_ahead([4, 5, 6])

def shrink(a)
  seen = []
  r = a.rfind { |x| seen << x; (a.pop; a.pop) if seen.size == 1; false }
  [r, seen]
end
p shrink([1, 2, 3, 4, 5])
p shrink(["a", "b", "c", "d", "e"])

def grow(a)
  seen = []
  r = a.rfind { |x| seen << x; a.push(x) if seen.size == 1; false }
  [r, seen]
end
p grow([1, 2, 3])
p grow(["a", "b"])

def clear_it(a)
  a.rfind { |x| a.clear if x == 3 || x == "y"; x == 1 || x == "x" }
end
p clear_it([1, 2, 3])
p clear_it(["x", "y"])

# the typed array, for the same answers
b = [1, 2, 3, 4, 5]
seen = []
r = b.rfind { |x| seen << x; (b.pop; b.pop) if seen.size == 1; false }
p [r, seen]
