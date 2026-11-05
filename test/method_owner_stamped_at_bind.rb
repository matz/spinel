# A Method read back out of a container, or out of a local written two
# Methods, answers the owner its bind site knew (a class method's is the
# class's singleton, an included method's the module), and the local its
# source_location.
module Greet; def hi = "hi"; end
class C
  include Greet
  def self.a = 1
  def self.b = 2
  def own = 3
end

ms = [C.method(:a), C.new.method(:hi), C.new.method(:own)]
ms.each { |m| p m.owner }
p ms[0].unbind.owner

m = C.method(:a)
m = C.method(:b) if C.a > 0
p m.owner
p m.source_location[1]
p m.call
