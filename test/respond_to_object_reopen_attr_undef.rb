# respond_to? on a reopened Object's attr_reader/attr_writer (public and
# private), and an undef in Object that hides a BasicObject reopen's
# method of the same name.
class Object
  attr_reader :extra
  attr_accessor :acc
  private attr_reader :pex
  private attr_writer :pw
end
class BasicObject; def bb = 3; def bc = 4; end
module Kernel; private def kq = 6; end
class Object
  undef_method :bb
  private def bc = 7
end
class A; end
class B; def bb = "own"; end
vals = [A.new, B.new, 3, "s", nil, [1]]
names = [:extra, :acc, :acc=, :pex, :pw=, :bb, :bc, :kq]
vals.each { |v| p names.map { |n| [v.respond_to?(n), v.respond_to?(n, true)] } }
names.each do |n|
  m = [n].first
  p vals.map { |v| [v.respond_to?(m), v.respond_to?(m, true)] }
end
