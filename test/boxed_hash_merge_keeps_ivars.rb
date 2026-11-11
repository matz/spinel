# Hash#merge answers a copy of its receiver, which takes the receiver's
# instance variables, as dup does, on a boxed receiver too
class Hash
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end

h = {}.tag!(1)
p h.tag
p h.merge(x: 1).tag
p h.merge({ y: 2 }, { z: 3 }).tag
p h.merge(x: 1)
p h.dup.tag

# a typed Hash
t = { a: 1 }
t.tag!(:t)
p t.merge(b: 2).tag
p t.merge(b: 2)

# a Hash without any: none on the copy
p({ k: 1 }.merge(j: 2).tag)

# the boxed receiver an Array (compact makes no copy of an Array's ivars)
class Array
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end
def pick(i) = [[1, nil].tag!(:a), { q: nil }.tag!(:h)][i]
p pick(0).compact.tag
p pick(1).compact.tag
p pick(1).merge(r: 1).tag
