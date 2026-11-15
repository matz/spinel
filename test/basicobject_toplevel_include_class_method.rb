# A method of a module included at the top level is a method of Object. An
# instance of a BasicObject subclass is not an Object, so a bare identifier
# in its instance method does not reach the module and is CRuby's NameError
# (a call written with parentheses is its NoMethodError,
# test/basicobject_toplevel_include_call.rb). The class itself is an
# Object: its class methods, and the blocks inside them, still reach the
# module, as do instances of ordinary classes and the top level.

module T
  module_function

  def twice
    yield + yield
  end

  def hello = "hi"
end

include T

class Blank < BasicObject
  def self.greet = hello
  def self.sum = twice { 2 }
  def self.each_sum = [1, 2].map { |x| twice { x } }

  def greet
    hello
  rescue ::NameError => e
    e.message
  end
end

class Plain
  def greet = hello
  def sum = [3].map { |x| twice { x } }
end

p Blank.greet, Blank.sum, Blank.each_sum
$stdout.puts Blank.new.greet
p Plain.new.greet, Plain.new.sum
p hello, twice { 5 }
