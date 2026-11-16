# A prepended initialize that drops the keyword rest is no forward for a
# builtin whose keywords take effect in initialize: Time's `in:` sets the
# offset there, so dropping it changes the value. Refused, not answered with
# the offset the keyword gave (it is a forward for String and Hash, whose
# `new` applies theirs itself).
module Drop
  def initialize(*a, **kw, &b) = super(*a, &b)
end
Time.prepend(Drop)
p Time.new(2000, 1, 1, in: "+09:00").utc_offset
