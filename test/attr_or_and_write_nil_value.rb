# `o.v &&= x` and `o.v ||= x` in value position answer the attribute: nil
# for `&&=` over a nil field, which a local then holds; `||=` is nil only
# where its value can be. The read after the write took the plain field,
# its nil bit unread.
class A
  attr_accessor :v
  def initialize(v) = @v = v
end
a = A.new(ARGV.size == 0 ? nil : 1)
y = (a.v &&= 9)
p y, a.v
x = (a.v ||= 5)
p x, a.v
b = A.new(3)
z = (b.v &&= 7)
p z, b.v
w = (b.v ||= 1)
p w
n = ARGV.size == 0 ? nil : 2
c = A.new(nil)
q = (c.v ||= n)
p q, c.v
