# An instance variable passed to a parameter that #localtime changes in
# place lends its slot: the change lands in the object, as in CRuby.
def to_tokyo(t) = t.localtime(32400)

class Event
  def initialize = @at = Time.utc(2026, 10, 15, 1, 0, 0)
  def localize = to_tokyo(@at)
  def offset = @at.utc_offset
end

e = Event.new
e.localize
p e.offset
