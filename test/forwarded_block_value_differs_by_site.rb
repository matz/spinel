# A method that forwards its block to a yielding method (`&`, `&b`, `...`)
# answers each call site's block value: the forwarding call is one node in
# its body, so blocks of different kinds at different sites make that value
# a boxed one rather than the first site's kind.

def find(name)
  block_given? ? yield(name) : "none:#{name}"
end

def anon(*, &) = find(*, &)
def named(x, &b) = find(x, &b)
def dots(...) = find(...)

p anon(:a) { |n| n.to_s * 2 }
p anon(:b) { |n| 42 }
p anon(:c)
p named(:d) { |n| [n] }
p named(:e) { |n| n.size + 0.5 }
p dots(:f) { |n| n }
p dots(:g) { |n| nil }

class V
  def self.machine(*, &)
    find(self, *, &)
  end

  def self.find(owner, name)
    block_given? ? yield(name) : "#{owner}:#{name}"
  end
end

p V.machine(:x) { |n| n.to_s * 2 }
p V.machine(:y) { |n| 42 }
p V.machine(:z)
