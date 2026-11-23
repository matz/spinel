# class-body locals: top, class, module, sclass, reopen, nested class, a Class.new and class << obj in a def
x = "top"
class A
  x = 1
  class B
    x = :b
    p x
  end
  p x
end
module M; x = 2.5; p x; end
class << A; x = [1]; p x; end
class A; x = "re"; p x; end
def mk
  x = "def"
  k = Class.new {
    x2 = 3
    define_method(:v) { x2 }
  }
  o = Object.new
  class << o; def hi = :hi; end
  [k.new.v, o.hi, x]
end
p mk, x
