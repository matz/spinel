# A builtin class's `new` that a program's `self.new` answers runs that
# method, beside an empty initialize of the class's own and beside a
# prepended forward with defaults: neither builds the builtin's value.
class Array
  def initialize(*) = nil
  def self.new(x) = [x]
end
p Array.new(2)
module Sized
  def initialize(d = 3, capacity: 1) = super
end
Hash.prepend(Sized)
class Hash
  def self.new(*a, **k) = [:own, a, k]
end
p Hash.new(5)
p Hash.new(capacity: 2)
