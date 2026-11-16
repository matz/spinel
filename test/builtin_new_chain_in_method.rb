# A `new` inside a method or a lambda counts a reopening only where the load
# record proves it made before the call can first run. Here the method runs
# first at a statement ahead of the reopening, so its `new` builds Array's own
# value, as CRuby's does; the top-level `new` after the reopening builds the
# empty one. Where the record cannot say, the chain is the one without the def.
def helper = 1
def mk = Array.new(2, 0)
lam = -> { Array.new(1, 7) }
p helper
p mk
p lam.call
class Array
  def initialize(*); end
end
p Array.new(2, 0)
