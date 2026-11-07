# A constant that aliases a class of the program (E = Mod::Err) answers the
# class's methods as the class does: its reflection (constants,
# instance_method, method_defined?, ...), its class methods, ==, <=, ===
# and a raise and a rescue through it.
module Mod
  class Err < StandardError
    X = 1
    def self.make = new("m")
    def hi = :hi
  end
end
E = Mod::Err
e = E.new("x")
p E === e, E === 1
p E.name, E.to_s, E.inspect
p E.constants
p E.method_defined?(:hi), E.instance_method(:hi).name
p E.respond_to?(:make), E.make.message
p E.ancestors.first, E.superclass
p E.instance_methods(false)
p E == Mod::Err, E.equal?(Mod::Err), E.hash == Mod::Err.hash
p E <= StandardError, E < Exception
p e.is_a?(E), e.kind_of?(E), e.instance_of?(E)
case e
when E then p :when_e
end
p E.method(:make).call.class
p E.public_method_defined?(:hi)
p E.const_get(:X)
p E.allocate.class
begin
  raise E, "r"
rescue E => x
  p [x.class, x.message]
end
