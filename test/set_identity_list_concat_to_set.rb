# In a program whose identity Set is handed on (so every Set's calls are
# checked), Sets made with to_set from lists joined with `+` are no
# refusal when both lists hold Symbols: a literal list, or the names a
# reflective list answers (`nil.methods`), which are Symbols each.
require "set"

class Visits
  def initialize = @seen = Set.new.compare_by_identity
  def seen = @seen
end

kept = Visits.new.seen
p kept.size

module Names
  NIL_ONES = (nil.methods + %i[!]).to_set.freeze
  JOINED = ([:a, :b] + %i[c] + [:a]).to_set
end
p Names::NIL_ONES.include?(:nil?), Names::NIL_ONES.include?(:!), Names::NIL_ONES.include?(:nope)
p Names::JOINED.size, Names::JOINED.include?(:c)
