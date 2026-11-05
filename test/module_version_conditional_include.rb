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
