# The defaults a prepended forward leaves unfilled run after the call's own
# arguments, its keywords' values too: Hash.new's `capacity:` value runs
# before the default the forward passes on.
$log = []
module Logged
  def initialize(d = ($log << :default; 5), **kw) = super
end
Hash.prepend(Logged)
h = Hash.new(capacity: ($log << :capacity; 3))
p h[:x], $log
$log.clear
g = Hash.new(capacity: 4)
p g[:x], $log
