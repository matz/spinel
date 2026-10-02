# A method named include in an unrelated class does not stand in for
# Object.include: the call is still refused.
class Foo
  def include(m) = :mine
end
module M; def mm = 7; end
Object.include M
p 1.mm
p Foo.new.include(1)
