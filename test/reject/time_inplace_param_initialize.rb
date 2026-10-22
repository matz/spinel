# An initialize's parameters are passed by value (sp_X_new), so a Time it
# changes in place by #localtime cannot reach the caller: refused.
# spinel: reject-time-inplace: whose parameters are passed by value
class Stamp
  def initialize(t) = t.localtime(3600)
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
Stamp.new(a)
p a.utc_offset
