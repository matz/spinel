# A builtin module mixed into the including class by a hook:
# a module extended ahead of Enumerable and a class method of the class come ahead of it.
module Finders
  def first = :finder_first
  def count = :finder_count
end
module M
  def self.included(base)
    base.extend(Finders)
    base.extend(Enumerable)
  end
end
class A
  include M
  def self.each
    yield 4
    yield 2
  end
  def self.min = :own_min
end
p A.sort, A.min, A.first, A.count, A.max
