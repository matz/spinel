# A Method of a module's class method it does not have raises NameError,
# which names the class 'Module' until something makes the module's
# singleton class, and '#<Class:V>' from then on (here a def self.). The
# module's instance method of the name is not its class method, nor a
# class's that does not extend it. A `&&=` of a variable reads it first and
# writes none that is unset: it makes no singleton class.
module V
  def b(x) = x * 2
end
class K
  include V
end
p((V.method(:b) rescue $!.class))
begin
  V.method(:b)
rescue NameError => e
  p e.message
end
p K.new.b(3)
module V
  def self.c = 2
end
begin
  V.method(:b)
rescue NameError => e
  p e.message
end
module F
  def f = 1
end
class K2
end
p((K2.method(:f) rescue $!.message))
module Q
  def b = 1
  @x &&= 1
  (@@y &&= 1) rescue nil
end
p((Q.method(:b) rescue $!.message))
