# A class body's call of a program method that defines and writes nothing
# when it runs is no code the record cannot follow; one whose definitions
# the macro form runs is recorded as them (defs); one that writes, or calls a
# method the walk has not recorded yet, is (opaque). Two methods that call
# each other and do nothing else (ping, pong) do nothing, a cycle or not.
class Foo
  def self.label(x) = "foo #{x}"
  def self.twice = label(1) + label(2)
  def self.defs = attr_accessor(:z)
  def self.ping = pong
  def self.pong = ping
  def self.sets = @count = 1
  def self.calls_later = later
  twice
  defs
  ping if false
  sets
  calls_later if false
  def self.later = 1
end
p Foo.twice
