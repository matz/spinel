# spinel: gc-stress
class Sink
  def accept(prefix: make_prefix, **opts)
    prefix << "seen"
    opts[:seen] = prefix.size
    opts
  end

  def make_prefix = ["start"]
end

class Other
end

def pick(n) = n == 1 ? Sink.new : Other.new

p Sink.new.accept(a: 0)
p pick(1).accept(a: 1, b: 2)
p pick(1).accept(prefix: ["given", "prefix"], a: 3)
options = { a: 4, b: 5 }
p pick(1).accept(**options)
