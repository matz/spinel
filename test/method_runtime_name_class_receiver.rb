# `method(name)` on a class, with the name known only at run time: the class's
# own class methods, from a `def self.` and from outside it.
class Tok
  def self.from_a(r) = r[:a]
  def self.from_b(r) = r[:b]

  def self.pick(r, *names)
    names.inject(nil) do |_, m|
      m = self.method(m) if m.is_a?(Symbol)
      v = m.call(r)
      break v if v
    end
  end
end

p Tok.pick({ b: "B" }, :from_a, :from_b)
p Tok.pick({ a: "A" }, :from_a, :from_b)
p Tok.pick({ a: "A" }, ->(r) { r.size }, :from_a)

begin
  Tok.pick({}, :from_z)
rescue NameError => e
  p e.class
end

def held(name) = Tok.method(name)

name = [:from_b, :from_a].first
p held(name).arity
p held(name).call({ b: 4 })
