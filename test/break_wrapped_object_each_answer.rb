# A block that may break wraps its call: when no break is taken, the call
# answers what its method answers. A program's Object, Kernel or NilClass
# `each` the plan binds on a builtin value answers its own value, not the
# receiver a builtin each answers; a builtin each still answers its
# receiver.
class Object
  def each
    yield 1
    :done
  end

  def each_with_index
    yield 1, 0
    10
  end
end

class NilClass
  def each
    yield 1
    42
  end
end

module Kernel
  def reverse_each
    yield 2
    "rev"
  end
end

class Foo
  def each
    yield 1
    :foo
  end
end

p(5.each { |x| break :br if x > 10 })
p(5.each { |x| break :br if x > 0 })
p(2.5.each { |x| break :br if x > 10 })
p(nil.each { |x| break 7 if x > 10 })
r = 5.each_with_index { |x, i| break 20 if x > 10 }
p r + 1
r = 5.each_with_index { |x, i| break 20 if x > 0 }
p r + 1
s = 2.5.reverse_each { |x| break "br" if x > 10 }
p s
p s.size
p(Foo.new.each { |x| break :br if x > 10 })
p([1, 2].each { |x| break :br if x > 10 })
p({a: 1}.each { |k, v| break :br if v > 10 })
p((1..2).each { |x| break :br if x > 10 })
p([3].reverse_each { |x| break :br if x > 10 })
