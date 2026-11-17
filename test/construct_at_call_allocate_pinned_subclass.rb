class P; attr_accessor :tag; end
class C < P
  def self.allocate = (o = super; o.tag = 1; o)
  def self.make = allocate
end
def mk = C.allocate
p mk.tag
p C.make.tag
class C
  def self.allocate = (o = super; o.tag = 2; o)
end
p mk.tag
p C.make.tag
