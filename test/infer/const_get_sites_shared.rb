# Three Object.const_get calls with a run-time name share one function of
# the arms over the program's constants (#12511), not a copy at each site.
class K1; def self.v = 1; end
class K2; def self.v = 2; end
class K3; def self.v = 3; end
class K4; def self.v = 4; end
class K5; def self.v = 5; end
class K6; def self.v = 6; end
class K7; def self.v = 7; end
class K8; def self.v = 8; end
class K9; def self.v = 9; end
class K10; def self.v = 10; end
class K11; def self.v = 11; end
class K12; def self.v = 12; end
names = %w[K1 K2 K7]
p Object.const_get(names[0]).v
p Object.const_get(names[1]).v
p Object.const_get(names[2]).v
