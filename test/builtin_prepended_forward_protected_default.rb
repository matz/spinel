# A default that calls a protected method runs where self is the object
# being built, which may call it, and is not moved to the call, where self
# is main: ahead of an empty initialize, the call builds the empty value.
class Array
  protected def secret = 5
end
module Seeded
  def initialize(n = 2, f = [].secret) = super
end
class Array
  prepend Seeded
  def initialize(*a) = nil
end
p((Array.new rescue $!.class))
p((Array.new(3) rescue $!.class))
