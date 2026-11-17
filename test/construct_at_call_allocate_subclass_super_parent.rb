class P; attr_accessor :tag; def self.allocate = (o = super; o.tag = :p; o); end
class C < P
  def self.allocate = (o = super; o.tag = [o.tag, :c1]; o)
end
f = -> { C.allocate.tag }
p f.call
class C
  def self.allocate = (o = super; o.tag = [o.tag, :c2]; o)
end
p f.call
class G < C; end
p G.allocate.tag
