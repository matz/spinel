# A local that may hold an untracked container joins no alias group, and
# still shares storage with the Array it does hold: `x` is `a`'s Array or
# an element's `steps` nothing fills, so a store through the caller's name
# reaches what `pick` returned.
class Route
  attr_reader :steps
  def initialize
    @steps = []
  end
end

def pick(routes, a)
  x = a
  routes.each { |r| x = r.steps }
  x
end

routes = []
shared = [1, 2]
y = pick(routes, shared)
shared << 3
p y.size
p y.equal?(shared)
p shared
