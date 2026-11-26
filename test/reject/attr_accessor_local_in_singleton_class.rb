# attr_accessor of a local in a class's `class << self`: the name is known
# only at run time
class K
  class << self
    n = :x
    attr_accessor n
  end
end

K.x = 1
p K.x
