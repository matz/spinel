# A class's own class method `===` answers `Klass === x`, `case`/`when`,
# `in`, grep and a class held as a value.
class Foo; def self.===(o) = o.is_a?(Integer) && o > 2; end
class Baz; end
class Kid < Foo; end

p Foo === 5, Foo === 1, Foo === "s"
p Foo.===(7)
p Kid === 9
p Baz === Baz.new, Baz === 1

ks = [Foo, Baz, Kid, Integer]
ks.each { |k| p [k, k === 5, k === 1, k === Baz.new] }

def chk(k, v) = k === v
p chk(Foo, 3), chk(Baz, Baz.new), chk(Baz, 3)

[1, "x", 7, Baz.new].each do |x|
  r = case x
      when Foo then :foo
      when Baz then :baz
      when String then :str
      else :other
      end
  p r
end

p(case [1, 2] when Foo then :foo when Array then :arr end)
r = case 4
    in Foo then :in_foo
    else :no
    end
p r
p [1, 3, "y", 8].grep(Foo)
p [1, 3, 8].count { |x| Foo === x }

module M; def self.===(o) = o.is_a?(String); end
p M === "a", M === 1
p(case "q" when M then :m else :no end)

class Always; def self.===(o) = true; end
p Always === [1, 2], Always === nil

# a class method `===` that answers nil runs, and does not match
class Lg; def self.===(o) = (puts "asked #{o.inspect}"; nil); end
p(case 3 when Lg then :y else :n end)
p Lg === 4
