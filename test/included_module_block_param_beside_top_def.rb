# A module the top level includes has a method with a block parameter, called
# with a literal block on a builtin value; a top-level def of its name follows.
module M
  def kb(&b) = [:module, b.call]
  def yb = [:module, yield]
end
include M

p 1.kb { 1 }
p "s".yb { 2 }
def kb = :top
def yb = :top
p 1.send(:kb)
p "s".send(:yb)
p kb
