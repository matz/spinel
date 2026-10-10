# The string collection budget carries a share of the object old generation,
# because the collection it pays for marks objects too. This program is the
# shape that needs it: it parses lines into records kept in one Array and
# throws every String away, so the string live set stays near zero while the
# object heap grows. Priced off the string heap alone the budget sits at its
# 256 KB floor, and every collection that buys re-reads the whole Array.
#
# SPINEL_GC_STR_BUDGET=str is the way back to that and `walk` carries the
# share whatever the gate says; the gc-str-budget-test leg reads all three on
# the same binary. The answer is the same in each. The default arm reports the
# share on its `[gc]` line, and the leg holds it between an eighth and a half
# of the object live set on that same line, so it fails with the share gone
# and with the whole heap in it. It has to collect less than half as often as
# `str` and about as often as `walk`: this is the end where the gate is open,
# and gc_str_budget_still.rb is the end where it is not.
#
# Single-threaded on purpose, as gc_obj_budget_walk.rb is: the string floor is
# per worker, and one worker keeps the worker count out of the numbers.
class Rec
  attr_reader :id, :qty
  def initialize(id, qty)
    @id = id
    @qty = qty
  end
end

recs = []
i = 0
while i < 300000
  line = "#{i},name-#{i % 1000},#{i * 7}"
  parts = line.split(",")
  recs << Rec.new(parts[0].to_i, parts[2].to_i)
  i += 1
end
total = 0
recs.each { |r| total = (total + r.qty - r.id) % 1000003 }
puts recs.size
puts total
