# A String, Integer or Float that may be nil, called with a name that the
# program's NilClass and Object both define: nil runs NilClass's method,
# any other value runs Object's.
class Object; def w(a = 1) = "obj#{a}"; def z = 5; end
class NilClass; def w(a = 2) = a * 3; def z = nil; end
def mk(f) = f ? 7 : nil
def mf(f) = f ? 1.5 : nil
def ms(f) = f ? "s" : nil
p mk(false).w, mk(true).w, mk(false).w(4), mk(true).w(4)
p mf(false).w, mf(true).w, ms(false).w, ms(true).w
p mk(false).z, mk(true).z
x = mk(ARGV.size > 3)
p((x.w + 1 rescue $!.class))
