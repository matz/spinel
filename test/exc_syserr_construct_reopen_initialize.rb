# CRuby's SystemCallError#initialize builds the errno message and then calls
# super with it, so a program's reopening of StandardError#initialize runs on
# the BUILT message however the Errno class is constructed: Errno::X.new,
# `raise Errno::X, msg`, SystemCallError.new, a class held in a variable. The
# arguments the call wrote (a second one, a function name) are the syserr
# initialize's, never the reopening's.
class StandardError
  def initialize(msg = "dflt", &blk)
    @seen = msg
    super("S[#{msg}]")
  end
  attr_reader :seen
end

p Errno::EACCES.new("a").message
p Errno::EACCES.new.message
p Errno::EACCES.new("a", "fn").message
e = Errno::ENOENT.new("x")
p e.message, e.seen, e.errno, e.is_a?(SystemCallError)

begin; raise Errno::EACCES, "b"; rescue => e; p [e.class, e.message, e.errno]; end
begin; raise Errno::EACCES; rescue SystemCallError => e; p e.message; end
begin; raise Errno::EACCES.new("z"); rescue => e; p e.message; end
def boom = raise(Errno::EPIPE, "pp")
begin; boom; rescue Errno::EPIPE => e; p e.message; end

p SystemCallError.new("x").message
p SystemCallError.new("x", 2).message
p SystemCallError.new("x", 2).class
begin; raise SystemCallError, "q"; rescue => e; p e.message; end
begin; raise SystemCallError.new("q", 13); rescue => e; p [e.class, e.message]; end

k = Errno::EPIPE
p k.new("w").message
p IO::EAGAINWaitReadable.new("z").message

# an error the runtime raises still runs it (K-058 shape 1)
begin; File.open("/nonexistent-spinel-dir/q"); rescue => e; p e.class, e.message; end
