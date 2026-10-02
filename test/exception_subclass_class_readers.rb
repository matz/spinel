# The readers CRuby defines on one class of the exception hierarchy
# (SystemCallError#errno, LoadError#path, KeyError#key, NameError#receiver,
# ...) called on an instance of a program's subclass held in a typed local:
# the call fell to the object dispatch, which knew none of them, and raised
# NoMethodError (a rescued, boxed instance already read them).

class S < Errno::ENOENT; end
class L < LoadError; end
class K < KeyError; end
class N < NameError; end
class NM < NoMethodError; end
class FE < FrozenError; end
class SE < StandardError; end
class Own < LoadError; def path; "mine"; end; end

s = S.new("m")
p s.errno, s.message, s.inspect, s.backtrace, s.cause
p s.exception.equal?(s), s.exception("w").message, s.exception("w").errno
p s == S.new("m"), s.full_message.include?("No such file or directory - m")
p (s.path rescue $!.class)

l = L.new("m")
p l.path, l.message, l.backtrace, l.cause, l.inspect, l.to_s
p l.exception.equal?(l), l == L.new("m"), l.exception("n").message
p (l.errno rescue $!.class)
p Own.new("m").path

k = K.new("m")
p (k.key rescue $!), (k.receiver rescue $!)
n = N.new("m")
p n.name, (n.receiver rescue $!), (n.key rescue $!.class)
nm = NM.new("m")
p nm.name, nm.args, nm.private_call?
p (FE.new("m").receiver rescue $!)
se = SE.new("m")
p (se.errno rescue $!.class), (se.path rescue $!.class), (se.key rescue $!.class)
p (StandardError.new("x").path rescue $!.class)
