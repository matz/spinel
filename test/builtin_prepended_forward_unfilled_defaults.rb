# A prepended initialize whose bare `super` passes on defaults the call
# leaves unfilled hands the builtin's own initialize their values: String's
# then copies a UTF-8 default rather than starting BINARY. A default that
# reads an earlier parameter reads the call's argument, which runs once, and
# the defaults run after the call's own arguments, in order.
$log = []
module EmptyText
  def initialize(s = +"", **kw) = super
end
String.prepend(EmptyText)
s = String.new
p s, s.encoding
t = String.new("ab")
p t, t.encoding

module Filled
  def initialize(n, v = ($log << :default; n * 10)) = super
end
Array.prepend(Filled)
a = Array.new(($log << :arg; 2))
p a, $log

module Span
  def initialize(first = ($log << :first; 1), last = ($log << :last; first + 3)) = super
end
Range.prepend(Span)
$log.clear
$calls = 0
r = Range.new(($calls += 1; 5))
p r, $calls, $log
$log.clear
q = Range.new
p q, $log
