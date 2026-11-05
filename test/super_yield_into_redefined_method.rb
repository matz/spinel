# `super` into a method a later definition replaces, where the parent yields
# and the child's caller passes the block: the block's value is what the
# parent's yield answers in each version, as the version in force at the call
# runs it. A child inlined into its caller carries the block down, so the type
# of that value is read from the blocks of the child's callers.

class P
  def m
    yield 1
  end
end
class C < P
  def m
    super
  end
end
def go(o)
  o.m { |x| x + 1 }
end
p go(C.new)
class P
  def m
    yield 5
    yield 6
  end
end
p go(C.new)

# the same on a class method
class A
  def self.m
    yield 1
  end
end
class B < A
  def self.m
    super
  end
end
def run
  B.m { |x| x.to_s + "1" }
end
p run
class A
  def self.m
    yield "s"
  end
end
p run

# two levels of super
class A2
  def m(n)
    yield n
  end
end
class B2 < A2
  def m(n)
    super
  end
end
class C2 < B2
  def m(n)
    super(n * 2)
  end
end
def run2(o)
  o.m(3) { |x| x * 1.5 }
end
p run2(C2.new)
class A2
  def m(n)
    yield n.to_s
  end
end
p run2(C2.new)

# a version an alias captured, which is renamed
class P3
  def m
    yield 1
  end
end
class C3 < P3
  def m
    super
  end
end
def run3(o)
  o.m { |x| x + 10 }
end
p run3(C3.new)
class P3
  alias n m
  def n
    yield 5
  end
  def m
    yield 7
  end
end
p run3(C3.new)
