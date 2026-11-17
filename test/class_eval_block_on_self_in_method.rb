# A block `class_eval` (and its kin) on self in a method runs the block with
# self as self; a self that is no Module raises CRuby's NoMethodError.
class K
  def self.tag(x) = puts("tag #{x}")
  def self.mk(n)
    class_eval do
      tag n
    end
  end
  class << self
    def mk2(n) = module_eval { tag(n * 2) }
  end
  def self.mk3(n) = class_exec(n) { |m| tag(m + 1) }
  def self.ie(n) = instance_exec(n) { |m| tag(m) }
end
module CM
  def mk4(n)
    class_eval do
      tag n
    end
  end
end
class L
  extend CM
  def self.tag(x) = puts("L #{x}")
end
module Say
  def say(n)
    class_eval do
      puts "say #{n}"
    end
  end
end
class Obj
  include Say
end
class Cls
  extend Say
end
K.mk(1)
K.mk2(2)
K.mk3(3)
K.ie(4)
L.mk4(5)
p K.mk2(1).nil?
Cls.say(6)
begin
  Obj.new.say(7)
rescue NoMethodError => e
  puts e.message
end
module Shadow
  Module = String
  NoMethodError = ArgumentError
  def run = class_eval { 42 }
end
class Sh
  extend Shadow
end
p Sh.run
class Nest
  def self.run
    Object.new.instance_exec { class_eval { 42 } }
  rescue ::NoMethodError => e
    e.message
  end
end
p Nest.run
module Named
  def go = class_eval { 1 }
end
class NamedObj
  include Named
end
begin
  NamedObj.new.go
rescue NoMethodError => e
  p [e.name, e.args, e.receiver.class]
end
class OwnRaise
  include Named
  def raise(*) = nil
end
begin
  OwnRaise.new.go
rescue NoMethodError => e
  p e.name
end
