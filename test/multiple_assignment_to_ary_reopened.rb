# A multiple assignment converts its value through the to_ary that runs
# when it runs: a method called before and after a reopen of the class
# takes the version defined by then, each time, as a nested target in a
# method's body does.

class K
  def to_ary = [1, 2]
end

def take(o)
  a, b = o
  [a, b]
end

def sum(o)
  a, b = o
  a + b
end

def nested(o)
  x, (y, z) = 0, o
  [x, y, z]
end

k = K.new
p take(k)
p sum(k)
p nested(k)

class K
  def to_ary = [3, 4]
end
p take(k)
p sum(k)
p nested(k)

class K
  def to_ary = [5, 6]
end
p take(k)
p sum(k)
p nested(k)
