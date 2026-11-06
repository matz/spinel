# A call made before a class's own method (with super) shadows an included
# one runs the module's method; after the def, the own method, whose super
# reaches the module's.
module M
  def v = 1
end

class C
  include M
end

o = C.new
puts o.v

class C
  def v = super + 10
end

puts o.v
