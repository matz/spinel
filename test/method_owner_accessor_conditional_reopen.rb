# A Method of an accessor whose class declares it again under a condition
# is owned by that class whichever declaration the bind meets: also where
# a superclass's reader stands behind the class's own, and where the other
# declaration is the Struct's member.
class C
  attr_reader :x
end
class C
  attr_reader :x if ARGV.empty?
end
m = C.new.method(:x)
p m.owner
p [C.new.method(:x), 1][0].owner
p m.call
class A; attr_reader :y; end
class B < A; attr_reader :y; end
class B; attr_reader :y if ARGV.empty?; end
p B.new.method(:y).owner
S = Struct.new(:a)
class S; attr_reader :a if ARGV.empty?; end
p S.new(1).method(:a).owner
