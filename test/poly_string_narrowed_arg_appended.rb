# A POLY variable narrowed to String by an is_a? (or nil) guard and handed to
# a method that appends to it: CRuby appends to the String the variable
# holds. The narrowed read unboxed a copy, and the append was lost.

def m2(s) = (s << "!"; nil)
def m1(s) = m2(s)
def up(s) = (s.upcase!; nil)
c = ARGV.size == 0
x = c ? +"s" : 1
m1(x) if x.kind_of?(String)
up(x) if x.instance_of?(String)
x.is_a?(String) && m2(x)
p x
unless !x.is_a?(String)
  m2(x)
end
p x
case x
when String then m2(x)
end
p x
def guarded(v)
  return nil if v.nil?
  m2(v)
  v.size
end
z = c ? +"n" : nil
p guarded(z), z
def pg(v)
  m1(v) if v.is_a?(String)
  v
end
w = c ? +"w" : 2
pg(w); p w
a = [+"a", 1, +"b"]
a.each_with_index { |v, i| m1(v) if v.is_a?(String) }
p a
h = {a: +"x", b: 1}
h.each_value { |v| m1(v) if v.is_a?(String) }
h.each { |k, v| m1(v) if v.is_a?(String) }
p h
q = c ? +"q" : 1
p(q.is_a?(String) ? q.upcase : 0)
class K
  def initialize(c); @v = c ? +"i" : 1; end
  def run; v = @v; m2x(v) if v.is_a?(String); p @v, v; end
  def m2x(s) = (s << "?"; nil)
end
K.new(true).run
