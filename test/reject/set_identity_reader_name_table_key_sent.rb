# spinel: reject-subclass: enters or is looked up in a Set, in a program that calls Set#compare_by_identity
# A key of a constant's Hash literal names a method that answers an
# identity Set, and the program walks the Hash's keys and sends each one:
# the name reaches the method, so the identity Set is not followed where it
# lives, and the Floats added through the send are refused.
require "set"

class Marks
  def initialize = @kept = Set.new.compare_by_identity
  def take(n) = keep(n)
  def keep(n) = @kept.add(n)
  def size = @kept.size
end

module Table
  HOOKS = { take: [1, 2] }.freeze
end

m = Marks.new
n = ARGV.size + 1
2.times { Table::HOOKS.each_key { |k| m.send(k, 1e300 * n) } }
p m.size
