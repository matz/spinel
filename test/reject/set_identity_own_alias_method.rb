# spinel: reject-subclass: enters or is looked up in a Set, in a program that calls Set#compare_by_identity
# A program that defines its own alias_method: a call of it may do anything
# with the names it is handed, here call the method that answers an
# identity Set, so the reader's name in that call reaches the reader, the
# identity Set is not followed where it lives, and the Floats added through
# it are refused.
require "set"

class Box
  def initialize = @seen = Set.new.compare_by_identity
  def seen = @seen
  def size = @seen.size
  def alias_method(new_name, old_name) = send(new_name)
  def trigger = alias_method(:seen, :unused)
end

b = Box.new
n = ARGV.size + 1
2.times { b.trigger << (1e300 * n) }
p b.size
