# A local that reads a class-level ivar holding a String -- in the class
# body, or through a class method's reader -- holds that String, as a local
# reading an instance's ivar does: an append to the ivar afterwards shows
# through it. Both printed the String as it was before the append.

class E; @y = +"e"; s = @y; @y << "!"; p s, s.equal?(@y); end

class C
  def self.set = (@x = +"c")
  def self.x = @x
  def self.grow = (@x << ".")
end
C.set
y = C.x
C.grow
p y, y.equal?(C.x)

module M
  def self.start = (@log = +"")
  def self.log = @log
  def self.add(m) = (@log << m; nil)
end
M.start
l = M.log
M.add("a"); M.add("b")
p l

# a reader whose ivar is never appended to still answers its String
class R; def self.set = (@v = +"r"); def self.v = @v; end
R.set
v = R.v
p v, v.frozen?
