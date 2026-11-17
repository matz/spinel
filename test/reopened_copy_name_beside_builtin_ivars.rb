# A reopened Array's or Hash's own method of a copy name (merge, compact,
# dup) answers what it returns, beside the builtin ivars the program
# writes on those values: no copy of the receiver's type is made of it.
class Hash
  def merge(o) = (o.size > 0 ? :merged : 0)
  def tag = @tag
  def tag=(v)
    @tag = v
  end
end
class Array
  def compact = (size > 1 ? "many" : 0)
  def tag = @tag
  def tag=(v)
    @tag = v
  end
end
h = { a: 1 }
h.tag = 1
x = h.merge({ b: 2 })
p x
p h.merge({})
a = [1, nil]
a.tag = 2
y = a.compact
p y
p [1].compact
p [h.tag, a.tag]
