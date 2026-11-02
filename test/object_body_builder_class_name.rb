# A class, Struct or module a reopened Object's body makes is a top-level
# constant's: CRuby names it without Object:: before it, as a module's own
# takes the module's name.
class Object
  B = Class.new
  S = Struct.new(:a)
  M2 = Module.new
  class Inner; end
end
module N
  B2 = Class.new
end
p B, S, M2, Inner, N::B2
p [B.name, S.name, M2.name, N::B2.name]
p S.new(1)
