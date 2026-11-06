# A bare `super` passes the method's own `**kw` on to the parent. When a
# call brings that `**kw` a hash of any key, the parent's `**kw` takes any
# key too. With a block, the parent that yields runs as its block-taking
# copy, which must take the same hash.
class B
  def m(a, **kw)
    yield
    [a, kw]
  end
end

class C < B
  def m(a, **kw) = super
end

class D < C
  def m(a, **kw) = super
end

empty = {}
str = {"s" => 1}
p C.new.m(1, **empty) { 1 }
p C.new.m(2, **str) { 1 }
p D.new.m(3, **str, z: 3) { 1 }
p B.new.m(4, k: 2) { 1 }

def fwd(o, &) = o.m(5, **{}, &)
p fwd(D.new) { 3 }

class B2
  def m(p1 = 51, p2, **kw)
    begin
      yield
      return [p1, p2, kw]
    ensure
      puts "ensure"
    end
  end
end

class C2 < B2
  def m(p1 = 51, p2, **kw) = super
end

def fwd2(&)
  h = {}
  C2.new.m(1, **h, &)
end
p(fwd2 { :blk })
