# A builtin module mixed into the including class by a hook:
# base.include(Comparable): the instance side compares.
module M
  def self.included(base)
    base.include(Comparable)
  end
end
class A
  include M
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
end
p A.new(1) < A.new(2), A.new(3).between?(A.new(1), A.new(5)), [A.new(2), A.new(1)].min.n
