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

# a class whose `===` only a `case` calls, with an Integer subject
class OnlyCase; def self.===(o) = o.is_a?(Integer); end
p(case 5 when OnlyCase then :y else :n end)
r = case 6
    in OnlyCase then :y
    else :n
    end
p r

# answers that are not true or false
class Word; def self.===(o) = o =~ /^\w+$/; end
p Word === "a b", Word === "ab"
p(case "hello" when Word then :word else :no end)
class Id; def self.===(o) = o; end
p(case 1 when Id then :y else :n end)
p(case nil when Id then :y else :n end)

# a class held in an Array, whose `===` takes a value of one kind
class Big; def self.===(o) = o > 2; end
p Big === 5
[Big, Integer].each { |k| p k === 3 }
class Sym; def self.===(o) = o == "x" ? :yes : nil; end
[Sym, Integer].each { |k| p [k === 3, k === "x"] }
p [3, "x", "y"].grep(Sym)

# other shapes of `===`: a rest parameter, an optional second parameter
class Rest; def self.===(*a) = a[0] == 1; end
p Rest === 1, Rest === 2
class Opt; def self.===(o, extra = 1) = o == extra; end
p Opt === 1
p(case 2 when Opt then :y else :n end)

# a class value tested against nil
class NilQ; def self.===(o) = o.nil?; end
kq = [1].size > 0 ? NilQ : Integer
p kq === nil, kq === 1
