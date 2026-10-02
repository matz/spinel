# SystemCallError#initialize builds the message from the class's errno:
# Errno::ENOENT.new("x") is "No such file or directory - x", a bare
# `raise Errno::ENOENT` is "No such file or directory", and
# SystemCallError.new("x", 2) is an Errno::ENOENT. The message was stored
# as given ("x", or the class name), and Errno::ENOENT.new was a NameError
# ("uninitialized constant ENOENT"). Only errnos whose number and text agree
# between Linux and macOS are printed.

class E3 < Errno::ENOENT; end
class E4 < Errno::ENOENT; def initialize(m, f); super(m, f); end; end
class E5 < Errno::ENOENT; def initialize(m); super("pre " + m); end; end
class E6 < Errno::ENOENT; def initialize; end; end
class E7 < Errno::ENOENT; def message; "own"; end; end
class E8 < Errno::ENOENT; def initialize(m); super; end; end
class E9 < Errno::ENOENT; def initialize; super; end; end
class E10 < Errno::EACCES; attr_reader :z; def initialize(m); super(m); @z = 7; end; end
class E11 < Errno::ENOENT; attr_accessor :w; end
class S < SystemCallError; end
class S4 < SystemCallError; def initialize(m); super(m, 2); end; end

# raise with a class, with and without a message
begin; raise Errno::ENOENT; rescue => e; p e.message, e.class, e.errno; end
begin; raise Errno::ENOENT, "foo"; rescue => e; p e.message, e.errno; end
begin; raise E3; rescue => e; p e.message, e.class, e.errno; end
begin; raise E3, "bar"; rescue => e; p e.message, e.errno; end
begin; raise Errno::ENOENT.new("x"); rescue => e; p e.message, e.errno; end
begin; raise SystemCallError, "m"; rescue => e; p e, e.errno; end
begin; raise SystemCallError; rescue ArgumentError => e; p e; end
begin; raise SystemCallError, 13; rescue => e; p e, e.errno; end
begin; raise E10, "zz"; rescue E10 => e; p e.message, e.errno, e.z; end
begin; raise E11, "yy"; rescue => e; p e.message, e.errno; end
begin; raise E11; rescue SystemCallError => e; p e.message, e.errno; end
begin; raise E6; rescue => e; p e.message, e.errno; end

# .new / .exception
p Errno::ENOENT.new("x").message, Errno::ENOENT.new.message, Errno::ENOENT.new("x").errno
p Errno::ENOENT.new("").message, Errno::ENOENT.new(nil).message
p Errno::ENOENT.new("a", "b").message, Errno::ENOENT.new(nil, "fn").message
p Errno::ENOENT.exception("u").message
p E3.new("q").message, E3.new("q").errno
p E4.new("x", "g").message, E4.new("x", "g").errno
p E5.new("k").message
p E6.new.message, E6.new.errno
p E7.new("x").message, E7.new("x").to_s
p E8.new("x").message, E9.new.message, E9.new.errno
e10 = E10.new("q"); p e10.message, e10.errno, e10.z
e11 = E11.new("r"); p e11.message, e11.errno
p [Errno::ENOENT, Errno::EACCES, Errno::EEXIST, Errno::EPIPE, Errno::EINVAL, Errno::ENOTDIR, Errno::EISDIR].map { |c| c.new("f").message }
p Errno::ENOENT::Errno, Errno::EACCES::Errno, Errno::EEXIST::Errno, Errno::EPIPE::Errno
p Errno::EAGAIN.new.message, Errno::EWOULDBLOCK.new("z").message
p IO::EAGAINWaitReadable.new("x").message
p IO::EAGAINWaitReadable.new("x").errno == Errno::EAGAIN::Errno

# SystemCallError.new(msg, errno = nil, func = nil) picks the Errno class
x = SystemCallError.new("x", 2)
p x.class, x.message, x.errno
p SystemCallError.new(nil, 2).message
p SystemCallError.new("x").message, SystemCallError.new("x").errno
p SystemCallError.new("x", 2, "fn").message, SystemCallError.new("x", 2, "fn").class
p SystemCallError.new(nil, nil).message, SystemCallError.new(nil).errno
p SystemCallError.new("q", 13).class
p SystemCallError.new(2), SystemCallError.new(2).errno
p SystemCallError.new("x", 2.7).errno, SystemCallError.new("x", 2.7).class

# what CRuby's initialize refuses
p (Errno::ENOENT.new(:sym) rescue $!)
p (Errno::ENOENT.new(5) rescue $!)
p (SystemCallError.new(:sym) rescue $!)
p (SystemCallError.new("a", "2") rescue $!)
p (SystemCallError.new rescue $!)
p (Errno::ENOENT.new("a", "b", "c") rescue $!)
p (SystemCallError.new("a", 2, "c", 4) rescue $!)
# a class directly under SystemCallError has no Errno constant: its
# initialize finds the Errno module and cannot convert it
begin; raise S; rescue Exception => e; p e.class, e.message; end
begin; raise S, "m"; rescue Exception => e; p e.class, e.message; end
p (S.new("m") rescue $!)
p (S4.new("z") rescue $!)

# through a Class value
k = Errno::EACCES
begin; raise k, "zz"; rescue => e; p e, e.errno; end
p k.new("q")
ks = [Errno::EPIPE, E3, SystemCallError]
ks.each { |c| begin; raise c, "m"; rescue => e; p [e.class, e.message, e.errno]; end }
ks.each { |c| p(c.new("v")) }
sk = SystemCallError
begin; raise sk, 2; rescue => e; p e, e.errno; end
def mk(k) = k.new("dyn")
p mk(Errno::EEXIST).message, mk(E3).message, mk(E3).errno

# the runtime's own raises keep their messages
begin
  File.open("/nonexistent/zz")
rescue Errno::ENOENT => e
  p e.message, e.errno, e.class
end
begin
  File.read("/nonexistent/zz")
rescue SystemCallError => e
  p e.message, e.errno, e.class
end

# the rendering readers
p Errno::ENOENT.new("x").inspect, Errno::ENOENT.new("x").to_s
p Errno::ENOENT.new("x") == Errno::ENOENT.new("x")
p Errno::ENOENT.new("x").detailed_message
p Errno::ENOENT.new("x").exception("n").message
e = Errno::ENOENT.new("x"); begin; raise e, "zz"; rescue => r; p r.message, r.errno; end

# raised into a fiber
fb = Fiber.new { begin; Fiber.yield; rescue => e; p e.message, e.errno, e.class; end }
fb.resume
fb.raise(Errno::ENOENT, "fx")
