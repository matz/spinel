# A builtin module mixed into the including class by a hook:
# the include written K.include(M) outside the class, a subclass with its own each, and is_a?(Enumerable).
module M
  def self.included(base)
    base.extend(Enumerable)
  end
end
class A
  def self.each
    yield 4
    yield 2
  end
end
A.include(M)
class B < A
  def self.each
    yield 9
  end
end
p A.sort, A.min, B.to_a, B.first, A.is_a?(Enumerable), B.kind_of?(Enumerable)
