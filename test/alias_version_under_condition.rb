# An alias made in a class reopening that a run-time condition skips (or
# runs) binds only where its branch ran: a call of the alias's name takes
# the version that ran, as a call of its target's name does. On a program
# class and on a reopened builtin, a program subclass of it included.
class Scalar < Numeric
  def initialize(v) = (@v = v)
end

$skip = true    # the guarded reopening does not run
$take = false   # the guarded reopening runs

class Foo
  def seconds = :foo1
  alias second seconds
end
unless $skip
  class Foo
    def seconds = :foo2
    alias second seconds
  end
end

class Bar
  def seconds = :bar1
  alias second seconds
end
unless $take
  class Bar
    def seconds = :bar2
    alias second seconds
  end
end

# an alias only, its target not redefined
class Baz
  def a = 1
  def b = 2
  alias c a
end
unless $skip
  class Baz
    alias c b
  end
end

class Numeric
  def seconds = [:s, self.class]
  alias :second :seconds
end
unless $skip
  class Numeric
    def seconds = [:s2, self.class]
    alias :second :seconds
  end
end

class Integer
  def mins = :m1
  alias minute mins
end
unless $take
  class Integer
    def mins = :m2
    alias minute mins
  end
end

f = Foo.new
p f.second
p f.seconds
b = Bar.new
p b.second
p b.seconds
p Baz.new.c
[1, 2.5, Scalar.new(3)].each { |x| p x.second; p x.seconds }
p 5.minute
p 5.mins
