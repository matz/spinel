# A bare `super` into Array from a method with a parameter after its rest
# parameter passes every parameter on, the rest as a splat: Array.new(*sizes,
# value). The method with the post parameter is initialize here, and the
# same shape reaches an ordinary method (`push`) of an Array subclass.
class Filled < Array
  def initialize(*sizes, value)
    super
  end
  def push(first, *mid, last)
    super
  end
end

f = Filled.new(3, 0)
p f
f.push(1, 2, 3, 4)
p f

# a destructured parameter, and a `(...)` method whose calls pass different counts
class Pairs < Array
  def push((a, b))
    super
  end
end
p Pairs.new.push([1, 2])

class Firsts < Array
  def first(...)
    super
  end
end
fs = Firsts.new([3, 4, 5])
p fs.first
p fs.first(2)
