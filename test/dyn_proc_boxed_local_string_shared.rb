# A String read out of a boxed local (one that also holds a Symbol, an
# Integer or nil, narrowed by is_a?, case or ===) and handed to a call on a
# boxed receiver: `@env[name]` with @env a slot that holds a Hash, or a proc,
# a lambda or a Method. The dispatch hands the local's box on, and a boxed
# String is its handle, so a proc that appends to its parameter appends to
# the caller's String. The call was refused as one that hands the proc a
# copy, also where the slot never holds a proc: a Hash lookup by a String
# out of a `case/in`.
class Env
  def initialize
    @env = {}
    @bang = ->(s) { s << "!" }
  end
  def set(v) = @env = v
  def bang = @bang
  def mark(s) = s.concat("?")
  def aref(e)
    name = e
    if name.is_a?(String)
      return @env[name] if @env.is_a?(Hash) && @env.key?(name)
      return @env[name]
    end
    name
  end
  def call(e)
    name = e.is_a?(Integer) ? nil : e
    case name
    when String then @env.call(name)
    else name
    end
  end
  def paren(e)
    name = e
    return @env.(name) if String === name
    name
  end
  def eqq(e)
    name = e
    return @env === name if name.is_a?(String)
    name
  end
end

e = Env.new
e.set({ "T" => "i64" })
p e.aref("T"), e.aref("U"), e.aref(:u8), e.aref(3)

s = +"a"
e.set(e.bang)
p e.aref(s), s
p e.call(s), s
p e.paren(s), s
p e.call(:sym), e.call(7)

t = +"b"
e.set(->(x) { x << "+"; x.size })
p e.aref(t), t
p e.eqq(t), t

u = +"c"
e.set(e.method(:mark))
p e.aref(u), u
p e.call(u), u

v = +"d"
e.set(proc { |x| x.insert(0, "<") })
p e.paren(v), v
