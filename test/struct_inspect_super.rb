# `super` in a Struct or Data class's own #inspect or #to_s, with no class
# of the program above it defining the name, is CRuby's Struct#inspect (to_s
# is its alias): the default #<struct ...> text. It raised "super: no
# superclass method". Also: a subclass that inherits such an #inspect shows
# its own name (the default text runs for its instance), with both inspect
# and to_s owned the super has a default of its own to reach, a member named
# to_s does not answer for it, and a struct holding itself stops as usual.
# The program's own struct_inspect or _struct_inspect is no default, and a
# super's block argument runs for its effect, here and in a plain class's
# super to Kernel#inspect.
Sup = Struct.new(:a) do
  def inspect = "S" + super
end
p Sup.new(2)
class Sub2 < Struct.new(:b)
  def inspect = "T" + super
end
p Sub2.new(3)
class Sub3 < Sup; end
p Sub3.new(4)
puts Sub3.new(5).to_s
TS = Struct.new(:c) do
  def to_s = "to_s:" + super
end
puts TS.new(6)
p TS.new(7)
Both = Struct.new(:e) do
  def inspect = "i:" + super
  def to_s = "s:" + super
end
p Both.new(8)
puts Both.new(9)
DD = Data.define(:x) do
  def inspect = "D" + super
end
p DD.new(x: 10)
Mem = Struct.new(:to_s, :k) do
  def inspect = "M" + super
end
p Mem.new("mem", 11)
puts Mem.new("mem2", 12).to_s
Rec = Struct.new(:r) do
  def inspect = "R" + super
end
r = Rec.new(nil); r.r = r
p r
n = Sup.new(Sup.new(1))
p n
x = [Sup.new(13), Sub3.new(14), 5][ARGV.size + 1]
p x
Own = Struct.new(:o) do
  def inspect = "i:" + super
  def to_s = "s:" + super
  def struct_inspect = "mine"
  def _struct_inspect = "under"
end
p Own.new(15)
puts Own.new(16)
p Own.new(17).struct_inspect, Own.new(18)._struct_inspect
Eff = Struct.new(:f) do
  def inspect = "E" + super(&(puts("side"); nil))
end
p Eff.new(19)
class Plain
  def inspect = "P" + super(&(puts("plain side"); nil)).sub(/0x\h+/, "X")
end
p Plain.new
