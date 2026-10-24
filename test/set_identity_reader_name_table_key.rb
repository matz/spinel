# A method that answers an identity Set (its last call adds to it) shares
# its name with a key of a constant's Hash literal, and with a member of a
# constant's list the program only asks membership of. The program only
# looks keys up in the Hash and asks the list `include?`, which answer
# values and truths, never the names, so the names reach no call: the
# identity Set is still followed where it lives, and the plain Set of
# Strings elsewhere is not refused.
require "set"

class Marks
  def initialize = @kept = Set.new.compare_by_identity
  def take(n) = keep(n)
  def enter(n) = keep(n)
  def keep(n) = @kept.add(n)
  def kept?(n) = @kept.include?(n)
end

module Table
  HOOKS = { take: [1, 2], other: [3] }.freeze
  NAMES = %i[enter leave].to_set.freeze
  def self.slots(k) = HOOKS[k]
  def self.slots?(k) = HOOKS.key?(k)
  def self.named?(k) = NAMES.include?(k)
end

m = Marks.new
a = Marks.new
m.take(a)
m.enter(a)
p m.kept?(a), m.kept?(Marks.new)
p Table.slots(:other), Table.slots?(:none), Table.named?(:leave)

tags = Set.new
tags << "red"
p tags.include?("red")
