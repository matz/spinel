# Singleton.__init__(klass) clears the class's instance: the next
# `instance` builds a new one. Another class's instance is left alone.
require "singleton"

class Registry
  include Singleton
  attr_accessor :n

  def initialize = @n = 0

  def self.reset!
    Singleton.__init__(self)
  end
end

class Other
  include Singleton
end

first = Registry.instance
first.n = 7
other = Other.instance
p Registry.reset!.equal?(Registry)
p Registry.instance.equal?(first)
p Registry.instance.equal?(Registry.instance)
p Registry.instance.n, first.n
p Other.instance.equal?(other)
p Singleton.__init__(Other).equal?(Other)
p Other.instance.equal?(other)
