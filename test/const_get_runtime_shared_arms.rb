# Run-time-name const_get and const_defined? from several sites on the same
# receiver share one set of arms; each site still answers its own name, and
# a receiver of another class keeps its own arms and miss message.
module Outer
  class Inner; def self.v = :inner; end
  LIMIT = 3
end
class Alpha; def self.v = :alpha; end
class Beta; def self.v = :beta; end
TOP = 42

names = %w[Alpha Beta TOP]
def pick(names, i) = names[i % names.size]

p Object.const_get(pick(names, 0)).v
p Object.const_get(pick(names, 1)).v
p Object.const_get(pick(names, 2))
p Object.const_defined?(pick(names, 0))
p Object.const_defined?("Nope" + "")
p Outer.const_get("Inner" + "").v
p Outer.const_get("LIMIT" + "")
p Outer.const_defined?("LIMIT" + "")

class Alpha
  def self.look(n) = const_get(n)
end
p Alpha.look("TOP" + "")

# a site in a method and one at the top level take the same arms
def find_const(n) = Object.const_get(n)
p find_const("Beta" + "").v
p find_const("TOP" + "")
p Object.const_get("Alpha" + "").v

begin
  Object.const_get("Missing" + "")
rescue NameError => e
  p e.message
end
begin
  Outer.const_get("Missing" + "")
rescue NameError => e
  p e.message
end
begin
  Object.const_get("lower" + "")
rescue NameError => e
  p e.message
end
