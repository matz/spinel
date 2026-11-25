# A subclass of Time does not stop the time package's class methods,
# which it inherits, from making a Time: their self is Time or the
# subclass.
require "time"
class Sub < Time
  def tag = "sub"
end
t = Time.parse("2001-02-03 04:05:06 +0000")
p [t.class, t.year, t.mon, t.hour, t.utc_offset]
s = Sub.parse("2001-02-03 04:05:06 +0900")
p [s.class, s.year, s.hour, s.utc_offset, s.tag]
p Time.iso8601("2020-01-02T03:04:05Z").year
p Sub.iso8601("2020-01-02T03:04:05Z").class
