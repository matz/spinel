# Mixins across files run in load order: a prepend after a required file's
# calls of the method reaches those calls only once it has run, a class
# reopened in a required file takes its include there, a `class << K` body
# there extends K, and a required file's top-level extend gives main its
# module's methods.
module Loud
  def speak = super.upcase
end
module Purr
  def talk = "purr, " + super
end
module Polite
  def greet = "good day, " + super
end
class Base
  def greet = "hi"
end
class Greeter < Base
end
class Cat
  def talk = "meow"
end
module Config
end
c = Cat.new
puts c.talk
puts Base.new.greet
require_relative "mixin_late_dog"
class Dog
  prepend Loud
end
puts Dog.new.speak
puts Dog.new.name
require_relative "mixin_late_cat"
puts c.talk
require_relative "mixin_late_polite"
puts Greeter.new.greet
puts Config.setting
puts helper
p Dog.ancestors.take(2), Cat.ancestors.take(2), Greeter.ancestors.take(3)
