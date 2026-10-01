# `self::Name` in a class method looks for the name in the class's ancestors
# by their simple names. Two modules named X (one nested) that include each
# other's simple name must not send that walk round forever, and
# `const_get` still reads the class's own private constant.

module X; end
module Y; include X; end
module Ns
  module X
    include Y
  end
end
class K
  include Y
  def self.g = self::KC
end
class L < K
  KC = 2
end
p L.g

class Other
  PK = 0
end
class A
  PK = 1
  private_constant :PK
  def self.read = const_get(:PK)
end
p A.read
