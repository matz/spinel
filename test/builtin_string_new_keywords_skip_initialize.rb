# String's own `new` given keywords builds the String from them itself and
# runs no initialize: neither a prepended module's nor a reopening's, whose
# defaults then never run either.
$log = []
module Logged
  def initialize(s = ($log << :default; +"x"), **kw)
    $log << :prepended
    super
  end
end
String.prepend(Logged)
class String
  def initialize(*a, **kw)
    $log << :reopened
  end
end
s = String.new("ab", capacity: 10)
p s, s.encoding, $log
t = String.new(capacity: 5)
p t, t.encoding, $log
u = String.new("cd", encoding: "BINARY")
p u, u.encoding, $log
v = String.new(encoding: "UTF-8")
p v, v.encoding, $log
