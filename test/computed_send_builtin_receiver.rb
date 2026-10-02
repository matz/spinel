# A send whose name is computed (`o.public_send("#{k}=", v)`) on a receiver
# known to be a builtin -- an IO, a String -- takes its arms from that
# class's own methods of the name's shape. Only a program object's or an
# untyped receiver's methods were candidates, so the send was refused.
def set(o, k, v) = o.public_send("#{k}=", v)
io = $stdout
p set(io, :sync, true)
p io.sync
p set(io, :sync, false)
p io.sync

def ask(s, q) = s.public_send("#{q}?")
p ask("abc", :empty), ask("", :empty)
p ask("ab", :ascii_only)
begin
  ask("abc", :nope)
rescue NoMethodError => e
  p e.class
end
