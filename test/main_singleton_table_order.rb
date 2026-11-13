# main's own to_s and inspect are its singleton class's: a module main
# extends comes after them, remove_method there takes them away (Object's
# answers then, until main defines the name again), and a program's
# instance_eval or instance_exec on main's chain is not the builtin that
# runs its block on main; one of another class, or a class method, is no
# method of main's, and a module BasicObject includes sits behind the builtin
class Object
  def to_s = "obj"
  def inspect = "obji"
end
module Ext
  def to_s = "ext"
end
extend Ext
p to_s
p inspect
p self.to_s
p [1].map { inspect }

class Foo
  def instance_exec(*) = 42
  def self.instance_eval = 1
end
p(instance_exec { to_s })
p(instance_exec { inspect })
module BI
  def instance_exec(*) = 0
end
class BasicObject
  include ::BI
end
p(instance_exec { inspect })

class << self
  remove_method :inspect
end
p inspect
p self.inspect
p to_s

def self.greet = "own"
class Object
  def greet = "objg"
end
p greet
class << self
  remove_method :greet
end
p greet
def self.greet = "again"
p greet

module IX
  def instance_exec(*, &b) = Object.new.instance_eval(&b)
end
extend IX
p(instance_exec { inspect })

def instance_eval(&b) = Object.new.instance_exec(&b)
p(instance_eval { to_s })
p(instance_eval { inspect })
