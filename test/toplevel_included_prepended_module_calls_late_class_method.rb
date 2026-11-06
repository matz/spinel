# A module prepended to a class is also included at the top level, where its
# original `go` runs as itself. The call in `go` meets `K.rec` before the
# class defines it, so it raises there; after the class defines it, the
# original and the prepended copy both bind it.
class K; end

module M
  def go(x) = K.rec(x)
end

include M

class Foo
  prepend M
end

begin
  p go(1)
rescue NoMethodError => e
  p [:nme, e.name]
end

class K
  def self.rec(x) = [:k, x]
end

p go(2)
p Foo.new.go(3)
