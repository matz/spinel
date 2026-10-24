# A method that answers an identity Set (its last call adds to it) shares
# its name with an alias another module defines (`alias take twice`,
# `alias_method :enter, :twice`). The alias's new name defines that
# module's own method and reaches no reader under another name, so the
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

module Plain
  def twice(n) = n * 2
  alias take twice
  alias_method :enter, :twice
end

class Other
  include Plain
end

m = Marks.new
a = Other.new
m.take(a)
m.enter(a)
p m.kept?(a), m.kept?(Other.new)
p Other.new.take(3), Other.new.enter(4)

tags = Set.new
tags << "red"
p tags.include?("red")
