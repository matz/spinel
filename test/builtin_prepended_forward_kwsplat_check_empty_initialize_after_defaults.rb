# The same check past a forward ahead whose default has an effect: that default runs first, then the binding raises.
$log = []
module Sized
  def initialize(*a, size: 1) = super
end
module Logged
  def initialize(x, d = ($log << :d; 3), **kw) = super
end
class Array
  prepend Sized
  prepend Logged
  def initialize(*); end
end
h = { extra: 1, more: 2 }
x = begin
  Array.new(5, **h)
rescue ArgumentError => e
  e.message
end
p x, $log
h = { size: 2 }
p Array.new(5, **h), $log
