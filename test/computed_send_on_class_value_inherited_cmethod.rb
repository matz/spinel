# A computed send on a Class value the program bounds answers a class method
# the class inherits from a superclass, as CRuby does. The receiver below
# holds A's or C's Class object, never Base's, and `self.hello` is Base's
# own: the send still reaches it on A and C.

class Base
  def self.hello = "hi from #{name}"
  class << self
    def bye = "bye from #{name}"
  end
end
class A < Base; end
class C < A
  def self.own = "own #{name}"
end
class Other
  def self.only_other = "other"
end

i = ARGV.size
name_of = ->(s) { (s + "").to_sym }

k = [A, C][i]
puts k.public_send(name_of.("hello"))
puts k.send(name_of.("bye"))
puts k.respond_to?(name_of.("bye"))

[A, C].each { |x| puts x.public_send(name_of.("hello")) }

h = {a: A, c: C}
puts h[:c].send(name_of.("hello"))
puts [C, A][i].send(name_of.("own"))

# a class method no class the receiver holds answers stays a NoMethodError
begin
  k.public_send(name_of.("only_other"))
rescue NoMethodError => e
  puts e.message
end
