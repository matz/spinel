# Two prepended forwards: the outer one takes a keyword rest and fills its
# default; the inner one names only `capacity`, so it raises `unknown keyword`
# as it binds, after the outer default has run.
$log = []
module Inner
  def initialize(d, capacity: 0) = super
end
module Outer
  def initialize(d = ($log << :o; 5), **kw) = super
end
class Hash
  prepend Inner
  prepend Outer
end
p((Hash.new(foo: 1) rescue $!.message), $log)
p Hash.new(capacity: 2)[:x], $log
