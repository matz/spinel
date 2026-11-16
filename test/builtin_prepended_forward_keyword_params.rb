# A prepended forward with keyword parameters: its bare `super` passes them
# on as keywords, the call's merged over the defaults, which run after the
# positional ones (a `**h` may pass any, so its defaults are values only);
# a builtin that takes keywords applies them.
$log = []
module Sized
  def initialize(d = ($log << :d; 3), capacity: 1) = super
end
Hash.prepend(Sized)
p Hash.new[:x], $log
p Hash.new(5)[:x], $log
p Hash.new(capacity: ($log << :v; 2))[:x], $log
p Hash.new(6, capacity: ($log << :w; 2))[:x], $log
h = {}
g = { capacity: 4 }
p Hash.new(7, **h)[:x], Hash.new(**g)[:x], $log
begin
  Hash.new(foo: 1)
rescue ArgumentError => e
  p e.message
end

module Encoded
  def initialize(s = +"", encoding: "UTF-8") = super
end
String.prepend(Encoded)
p String.new, String.new.encoding, String.new("a").encoding
p String.new("b", encoding: "ASCII-8BIT").encoding

# ...and where an empty initialize stands behind it, the keywords are its
# arguments, which it drops.
module Counted
  def initialize(n = 2, k: ($log << :k; 1)) = super
end
class Array
  prepend Counted
  def initialize(*a) = nil
end
p Array.new, Array.new(4, k: 5), $log
