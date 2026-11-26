# A super that reaches the builtin's own answer, in a class a Class.new
# evaluated more than once has below it, answers the classes made at run
# time: the default to_s names the object's class, and subclasses lists
# each class made.
class TS
  def to_s = super
end
mkt = -> { Class.new(TS) }
ta = mkt.call
tb = mkt.call
p ta.new.to_s.start_with?("#<#<Class:0x"), tb.new.to_s.start_with?("#<#{tb}:0x"), TS.new.to_s.start_with?("#<TS:0x")
TA = ta
p ta.new.to_s.start_with?("#<TA:0x"), "#{tb.new}".start_with?("#<#<Class:0x")
class SK
  def self.subclasses = super
end
class SS < SK; end
mks = -> { Class.new(SK) }
c = mks.call
d = mks.call
p SK.subclasses.size, SK.subclasses.include?(c), SK.subclasses.include?(SS), (SK.subclasses - [SS]).first == d
def subs(k) = k.subclasses
p subs(SK).include?(c), SS.subclasses
