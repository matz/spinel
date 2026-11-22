# A builtin module mixed into the including class by a hook:
# base.extend(Enumerable): the class side enumerates over its own each.
module M
  def self.included(base)
    base.extend(Enumerable)
  end
end
class A
  include M
  def self.each
    yield 3
    yield 1
  end
end
p A.sort, A.map { |x| x + 1 }, A.include?(3), A.first
