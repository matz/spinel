# A class method a program defines under a reflection name of Module's
# (constants, class_variables, const_defined?) answers for its own class or
# module, as in CRuby: the builtin's answer stood in for it (constants and
# class_variables answered [], const_defined? a value of another type).
# A class without such a method keeps the builtin's.
module Names
  def self.constants = ["first"]
  def self.ancestors = [:a]
  def self.class_variables = [:cv]
  def self.const_defined?(n) = :maybe
end
class Kept
  X = 1
  @@v = 2
  def self.cv = @@v
end
class Own
  Y = 1
  def self.constants(*) = [:mine]
end
p Names.constants, Names.ancestors, Names.class_variables, Names.const_defined?(:Z)
p((Names.constants + %i[second]).size)
p Kept.constants, Kept.class_variables, Kept.const_defined?(:X), Kept.cv
p Own.constants, Own.constants(false)
