# A class method of the program's answers a call before a Class row does:
# Module#include? and Class#inherited are the core's when the class defines
# none of its own, and the class's when it does. The hook the program
# defines on some class is a no-op for a class made of a Struct, a core
# class or an alias of a class without one.

class Own
  def self.include?(x) = 42
  def self.inherited(k) = "own inherited"
end
class Plain; end
module Mixin; end
class Mixed; include Mixin; end

p Own.include?(Comparable)
p Own.inherited(Plain)
p Plain.include?(Comparable)
p Mixed.include?(Mixin)
p Plain.__send__(:inherited, Own)

Base = Struct.new(:a)
class StructSub < Base; end
class Failure < StandardError; end
Alias = Plain
class AliasSub < Alias; end
Hooked = Own
class HookedSub < Hooked; end
puts "done"
