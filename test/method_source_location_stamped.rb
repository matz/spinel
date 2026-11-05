# A Method's source_location: an accessor's is its attr_* call's, and its
# path a frozen String, also for a local written two Methods, whose bind
# sites stamp it.
class C
  attr_accessor :z
  attr_reader :y
  def self.a = 1
  def self.b = 2
end
S = Struct.new(:x)

def show(s) = p(s ? [File.basename(s[0]), s[1]] : s)

o = C.new
show o.method(:z).source_location
show o.method(:z=).source_location
show S.new(1).method(:x).source_location

m = o.method(:z=)
m = o.method(:y) if C.a > 0
show m.source_location
m = C.method(:b)
m = C.method(:a) if C.a > 0
s = m.source_location
p s[0].frozen?
begin
  s[0] << "!"
rescue FrozenError => e
  p e.class
end
p C.method(:a).source_location[0].frozen?
