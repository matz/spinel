# String#replace with an argument that is nil at run time (a String-or-nil
# such as byteslice answers) raises TypeError, on a plain String and on one
# held as a handle, in statement and value form. A frozen receiver raises
# FrozenError first.
def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

s = +""
d = +"x"
try("replace(byteslice nil)") { d.replace(s.byteslice(1, -2)); d }
try("replace(nil)") { (+"y").replace(nil) }
n = nil
try("replace(nil local)") { (+"z").replace(n) }
h = String.new(capacity: 4)
h << "q"
try("handle replace(nil)") { h.replace(s.byteslice(5, 1)); h }
try("replace(1)") { (+"w").replace(1) }
plain = +"plain"
try("plain replace(byteslice nil)") { plain.replace(s.byteslice(9, 1)) }
r = (+"rr").replace(s.byteslice(0, 0))
try("value form") { r }
fz = "frozen".freeze
try("frozen and nil") { fz.replace(s.byteslice(9, 1)) }
hz = String.new(capacity: 4)
hz << "f"
hz.freeze
try("frozen handle and nil") { hz.replace(s.byteslice(9, 1)) }
try("expr form") { x = h.replace(s.byteslice(9, 1)); x }
