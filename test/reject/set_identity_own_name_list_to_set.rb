# spinel: reject-subclass: enters or is looked up in a Set, in a program that calls Set#compare_by_identity
# In a program whose identity Set is handed on, a Set made with to_set from
# a reflective name list joined with a list of Symbols is refused when the
# program defines that list's method itself, here as a class method that
# answers Strings: its answer is not the Symbols CRuby's would be.
require "set"

class Visits
  def initialize = @seen = Set.new.compare_by_identity
  def seen = @seen
end

module Names
  def self.constants = ["first"]
end

kept = Visits.new.seen
p kept.size
ALL = (Names.constants + %i[second]).to_set
p ALL.size
