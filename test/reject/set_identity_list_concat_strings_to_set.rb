# spinel: reject-subclass: enters or is looked up in a Set, in a program that calls Set#compare_by_identity
# In a program whose identity Set is handed on, a Set made with to_set from
# lists joined with `+` is still refused when one of them holds String
# literals, whose identity is not CRuby's.
require "set"

class Visits
  def initialize = @seen = Set.new.compare_by_identity
  def seen = @seen
end

kept = Visits.new.seen
p kept.size
JOINED = (%i[a b] + %w[c d]).to_set
p JOINED.include?("c")
