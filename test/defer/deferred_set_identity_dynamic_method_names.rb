# Compiled with --defer-refusals: a computed `method(name)` makes an arm
# for every name its receivers' classes define, compare_by_identity among
# them where a class of the program's defines it, and a receiver that may
# be an instance of a class with a `method` of its own splits each arm on
# the classes that have one. Those arms' symbols are no name the program
# spells, so the builtin wrapper an arm reaches compare_by_identity through
# makes no Set an identity one, and Strings enter the program's Sets as in
# CRuby. Without the fix the Set below raises NotImplementedError.
# spinel: defer-refusals: 3:true false 1 7
require "set"
class Headers
  def compare_by_identity = raise(TypeError, "Headers cannot compare by identity")
end
class Wrapper
  def method(name) = "wrapped #{name}"
end
class Box
  def initialize(v) = @v = v
  def value = @v
end
NAMES = %w(alias and begin).to_set.freeze
def m_of(o, n) = o.method(n)
puts NAMES.include?("and")
puts NAMES.include?("or")
s = Set.new
s << "a" << "a".dup
puts s.size
puts Box.new(7).value
if s.empty?   # (not taken: the calls type m_of)
  m_of(Wrapper.new, :x)
  m_of(Set[1], :size)
end
proc { "a" }.ruby2_keywords
puts "not reached"
