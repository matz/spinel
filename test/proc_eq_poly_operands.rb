# Two Procs compared as boxed values (a wrapper that holds its subject in an
# ivar and receives the other as an untyped argument, as an mspec-style
# `x.should.eq(y)` does) are equal exactly when the typed == says so: the same
# Proc or its dup or clone, never another Proc of the same body text. eql? and
# hash agree, so a dup finds the key its original made.
class Expect
  def initialize(name, v)
    @name = name
    @v = v
  end
  def eq(o)
    puts "#{@name}: #{@v == o} #{@v != o} #{@v.eql?(o)} #{@v.hash == o.hash}"
  end
end
class Object
  def should(name)
    Expect.new(name, self)
  end
end
pr = proc { |x| x }
l = lambda { |x| x }
a = lambda { |x, y| x + y }
pr.should("proc/itself").eq(pr)
pr.should("proc/dup").eq(pr.dup)
pr.should("proc/clone").eq(pr.clone)
pr.should("proc/dup of dup").eq(pr.dup.dup)
pr.should("proc/another proc").eq(proc { |x| x })
pr.should("proc/lambda").eq(l)
l.should("lambda/dup").eq(l.dup)
a.curry.should("curry/another curry").eq(a.curry)
c = a.curry
c.should("curry/itself").eq(c)
h = { pr => :p }
puts h[pr.dup].inspect
puts h[proc { |x| x }].inspect
x = [pr, 1].first
puts({ x => :boxed }[pr.clone].inspect)
