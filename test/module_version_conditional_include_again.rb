# An include under a condition, then another include of the module: the
# later one adds the module where it runs whether or not the first one
# ran, so a call after it runs the module's method.
module V1; def a = 1; end
class P1; def a = :p; end
class K1 < P1; end
if ARGV.size > 5
  class K1; include V1; end
end
p K1.new.a
class K1; include V1; end
p K1.new.a
module V1; def a = [super, 2]; end
p K1.new.a

# two modules under the condition, then one of them alone: only that one
# is added
module V2; def a = 1; end
module W2; def b = :w; end
class P2; def a = :p; def b = :q; end
class K2 < P2; end
if ARGV.size > 5
  class K2; include V2, W2; end
end
class K2; include V2; end
p [K2.new.a, K2.new.b]

# a module the conditional include brought through another one
module V3; def a = 1; end
module W3; include V3; def b = :w; end
class P3; def a = :p; def b = :q; end
class K3 < P3; end
if ARGV.size > 5
  class K3; include W3; end
end
class K3; include V3; end
p [K3.new.a, K3.new.b]
class K3; include W3; end
p [K3.new.a, K3.new.b]

# a conditional include's method yields, and a Method of a method that
# does not names the owner of the method that runs
module V4; def a = yield(10); end
class P4; def a = yield(3); end
class K4 < P4; end
k = K4.new
p k.a { |x| x + 1 }
if ARGV.empty?
  class K4; include V4; end
end
p k.a { |x| x + 1 }
module V4; def a = [yield(20), super]; end
p k.a { |x| x + 1 }

module V5; def b = 1; end
class P5; def b = :p; end
class K5 < P5; end
l = K5.new
p l.method(:b).owner
if ARGV.empty?
  class K5; include V5; end
end
p l.method(:b).owner
module V5; def b = [super, 2]; end
p [l.method(:b).owner, l.method(:b).call]
