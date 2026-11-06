# An include under a condition: a call after it runs the version the
# module holds when the call runs, told by the module's cells, and a super
# in a later version reaches past the module.
module V1; def a = 1; end
class P1; def a = :p; end
class K1 < P1; end
p K1.new.a
if ARGV.empty?
  class K1; include V1; end
end
p K1.new.a
module V1; def a = [super, 2]; end
p K1.new.a

# the later version calls no super
module V2; def a = 1; end
class P2; def a = :p; end
class K2 < P2; end
p K2.new.a
if ARGV.empty?
  class K2; include V2; end
end
p K2.new.a
module V2; def a = 2; end
p K2.new.a

# Plain copies: an include that may not run whose copies are not guarded
# (a reader asks for the module's methods, or two such includes call
# super), an extend under a condition and an include in a proc copy the
# module's later version as a layer of its own, and the call plan takes
# no entry over the module's versions there; a call made after the later
# version runs it.

# a conditional include of a module whose presence a reader asks
module V3; def a = 1; def c = :c; end
class P3; def a = :p; end
class K3 < P3; end
if ARGV.empty?
  class K3; include V3; end
end
module V3; def a = 2; end
k = K3.new
p [k.respond_to?(:c), k.a]

# two conditional includes whose methods call super
module V4; def a = [:v, super]; end
module W4; def a = [:w, super]; end
class P4; def a = :p; end
class K4 < P4; end
if ARGV.empty?
  class K4; include V4; end
  class K4; include W4; end
end
module V4; def a = :v2; end
p K4.new.a

# a conditional extend
module V5; def a = 1; end
class P5; def self.a = :p; end
class K5 < P5; end
if ARGV.empty?
  class K5; extend V5; end
end
module V5; def a = 2; end
p K5.a

# an include in a proc
module V6; def a = 1; end
class P6; def a = :p; end
class K6 < P6; end
pr = proc { class K6; include V6; end; K6 }
pr.call
module V6; def a = 2; end
p K6.new.a
