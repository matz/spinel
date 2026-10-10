# The other end of the string budget's gate from gc_str_budget_objects.rb.
#
# The budget carries a share of the object old generation only when the MARK is
# what a collection costs. This program holds a large object heap it never
# writes again and then throws away one small Array and one short String per
# turn: a minor collection finds nothing to mark and nothing to re-read, so
# the sweep is what those collections cost and the gate declines the share.
# Only the cycle after a full mark carries it, since the gate reads the last
# collection. SPINEL_GC_STR_BUDGET=walk carries it always, and the
# gc-str-budget-test leg compares the two arms of the same binary: the default
# has to collect at least half again as often as `walk`, or the gate is not in
# the budget.
#
# Single-threaded on purpose, for the same reason as the sibling.
class Rec
  attr_reader :id
  def initialize(id)
    @id = id
  end
end

live = []
i = 0
while i < 300000
  live << Rec.new(i)
  i += 1
end
sum = 0
k = 0
while k < 2000000
  a = [k, k + 1]
  s = "x#{k}"
  sum += s.size + a.length
  k += 1
end
puts live.size
puts sum
