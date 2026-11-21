# A bare `constants` in a class method lists the constants of the class
# the method was called on, a subclass's own first. In a class body it
# lists those assigned so far. (Two constants of one module come sorted
# here: CRuby lists them in the order of its symbol table, which is not
# the order they were written in and moved between 4.0.9 and 4.0.10.)

module Verbs
  GET = "GET"
  POST = "POST"
  def self.list = constants
  def self.own = constants(false)
  def self.each
    constants.sort.each { |name| yield name.downcase, const_get(name) }
  end
end

p Verbs.list.sort
p Verbs.own.sort
Verbs.each { |n, v| puts "#{n} #{v}" }

class Base
  A = 1
  SO_FAR = constants
  def self.list = constants
  def self.own = self.constants(false)
end

class Sub < Base
  B = 2
  OWN_SO_FAR = constants(false)
end

p Base.list
p Sub.list
p Sub.own
p Base::SO_FAR
p Sub::OWN_SO_FAR
