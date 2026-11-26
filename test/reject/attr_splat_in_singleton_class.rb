# attr_accessor with a splat of a local in a class's `class << self`
class K
  class << self
    names = [:y]
    attr_accessor(*names)
  end
end

K.y = 2
p K.y
