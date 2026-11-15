# A `new` on a class value runs, for each class the value may hold, the
# initialize the program has defined where the call runs, not the last one:
# a class opened in a loop block, a class chosen at run time, and a splat
# call, each before and after the class's initialize is reopened with
# another parameter type.
# A is the control: its initialize is never reopened.

class A
  attr_reader :v
  def initialize(x) = @v = x + 1
end
class B
  attr_reader :v
  def initialize(x) = @v = x * 10
end
[A, B].each { |k| p k.new(1).v }
class B
  def initialize(x) = @v = x.to_s
end
[A, B].each { |k| p k.new(2).v }

# a class chosen at run time
class C
  attr_reader :tag, :n
  def initialize(n)
    @tag = :a1
    @n = n
  end
end
class D
  attr_reader :tag, :n
  def initialize(n)
    @tag = :b
    @n = n
  end
end
k = ARGV.empty? ? C : D
p [k.new(3).tag, k.new(3).n]
class C
  def initialize(n)
    @tag = :a2
    @n = n + 1
  end
end
p [k.new(3).tag, k.new(3).n]
k = ARGV.empty? ? D : C
p [k.new(3).tag, k.new(3).n]

# a class value and a splat
class E
  attr_reader :v
  def initialize(a) = @v = a
end
args = [1]
ek = [E][0]
p ek.new(*args).v
class E
  def initialize(a, b = 5) = @v = a + b
end
p ek.new(*args).v
