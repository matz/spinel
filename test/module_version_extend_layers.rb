# A class that extends two modules holds the copies of the earlier
# module's versions behind the later module's copy: the later one answers,
# and its super reaches the version the earlier module holds when it runs.
module V1; def a = 1; end
module W1; def a = 2; end
class K1; extend V1; extend W1; end
p K1.a
module V1; def a = 3; end
p K1.a

module V2; def a = [:v1, super]; end
module W2; def a = [:w, super]; end
class P2; def self.a = :p; end
class K2 < P2; extend V2; extend W2; end
p K2.a
module V2; def a = [:v2, super]; end
p K2.a

# a block the later module's super hands on reaches an earlier version
# that yields
module V3; def a = [:v1, yield]; end
module W3; def a = [:w, super]; end
class K3; extend V3; extend W3; end
p K3.a { 5 }
module V3; def a = [:v2]; end
p K3.a { 5 }

# a later extend: until it runs, a call reaches the superclass's class
# method, and after it the version the module holds then
module V4; def a = [:v1, super]; end
class P4; def self.a = :p; end
class K4 < P4; def self.b = a; end
p K4.b
class K4; extend V4; end
p K4.b
module V4; def a = [:v2, super]; end
p K4.b

# two later extends: the later one's super reaches the version the
# earlier module holds when it runs
module V5; def a = [:v1, super]; end
module W5; def a = [:w, super]; end
class P5; def self.a = :p; end
class K5 < P5; end
p K5.a
class K5; extend V5; end
class K5; extend W5; end
p K5.a
module V5; def a = [:v2, super]; end
p K5.a
