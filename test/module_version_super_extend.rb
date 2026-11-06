# An extended module's versions of a method are versions of the class
# method: a super in either reaches the superclass's class method, not the
# module's earlier version.
module V1; def a = 1; end
class P1; def self.a = :p; end
class K1 < P1; extend V1; end
p K1.a
module V1; def a = [super, 2]; end
p K1.a

# the versions' class-level instance variables are the class's
module V2
  def a = (@n ||= 0) + 1
  def a = [super, (@m ||= 5)]
end
class P2; def self.a = :p; end
class K2 < P2; extend V2; end
p K2.a
