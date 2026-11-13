# A method named `_new_members` beside a Struct whose `new` runs both its
# members constructor (a site before its initialize) and its initialize:
# the method keeps its own name and the constructor its own.
S = Struct.new(:a)
x = S.new(1)
class S
  def initialize(a) = super(a * 10)
  def _new_members = 42
  def self._new_members = 43
end
p [x.a, S.new(1).a, x._new_members, S._new_members]
