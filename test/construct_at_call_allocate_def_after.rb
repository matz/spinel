class K
  attr_accessor :v
end
a = K.allocate
p a.class
class K
  def self.allocate
    o = super
    o.v = :custom
    o
  end
end
p K.allocate.v
p a.v
