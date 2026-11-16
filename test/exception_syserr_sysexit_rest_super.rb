# A rest, a **rest or a splat into the initialize of a SystemCallError or a
# SystemExit subclass (a bare super, super(*a, **k)) and a splat into their
# constructors run as CRuby runs them: the arguments are counted at run time,
# a non-empty keyword Hash is one more argument (both initializers are written
# in C and take no keywords), an empty one is none, and a block is ignored.
def show(label)
  e = yield
  extra = e.is_a?(SystemExit) ? " status=#{e.status.inspect}" : (e.is_a?(SystemCallError) ? " errno=#{e.errno.inspect}" : "")
  puts "#{label}: #{e.class} #{e.message.inspect}#{extra}"
rescue => x
  puts "#{label}: raises #{x.class}: #{x.message}"
end


class A1 < Errno::ENOENT; def initialize(*a); super(*a); end; end
class A2 < Errno::ENOENT; def initialize(m, *r); super; end; end
class A3 < Errno::ENOENT; def initialize(*a, **k); super; end; end
class A4 < Errno::ENOENT; def initialize(msg:); super; end; end
class A5 < Errno::ENOENT; def initialize(**k); super(**k); end; end
class A6 < Errno::EPIPE
  attr_reader :tag
  def initialize(tag, *a); @tag = tag; super(*a); end
end
class A7 < SystemCallError; def initialize(*a); super(*a); end; end
class B1 < SystemExit; def initialize(*a); super(*a); end; end
class B2 < SystemExit; def initialize(*a, **k); super; end; end
class B3 < SystemExit; def initialize(s, *r); super; end; end
class B4 < SystemExit; def initialize(msg:); super; end; end
class B5 < SystemExit; end

class C1 < Errno::ENOENT; def initialize(*a, &b); super; end; end
class C2 < SystemExit; def initialize(*a, &b); super(*a, &b); end; end
none = []
one = ["f"]
two = ["f", "open"]
show("A1 0") { A1.new(*none) }
show("A1 2") { A1.new(*two) }
show("A1 3") { A1.new("a", "b", "c") }
show("A2 1") { A2.new("x") }
show("A2 3") { A2.new("x", "y", "z") }
show("A3 kw") { A3.new("x", foo: 1) }
show("A3 nokw") { A3.new("x", "y") }
show("A4") { A4.new(msg: "x") }
show("A5 empty") { A5.new }
show("A5 kw") { A5.new(a: 1) }
show("A6") { e = A6.new(:t, *one); puts e.tag.inspect; e }
show("A7") { A7.new("x") }
show("B1 0") { B1.new(*none) }
show("B1 2") { B1.new(*[4, "m"]) }
show("B1 str") { B1.new(*["only"]) }
show("B1 f") { B1.new(*[2.7, "m"]) }
show("B2 kw") { B2.new(1, a: 2) }
show("B2 nokw") { B2.new(true, "ok") }
show("B3") { B3.new(false, "no") }
show("B4") { B4.new(msg: "x") }
show("B5 splat") { B5.new(*[3, "m"]) }
show("SE splat") { SystemExit.new(*[5, "z"]) }
show("ENOENT splat") { Errno::ENOENT.new(*two) }
show("ENOENT kw") { h = {}; Errno::ENOENT.new("x", **h) }
show("SCE splat") { SystemCallError.new(*["m", 2]) }
show("raise") { begin; raise A1.new(*two); rescue SystemCallError => e; e; end }
show("C1") { C1.new("x") }
show("C2") { C2.new(1) { 2 } }
