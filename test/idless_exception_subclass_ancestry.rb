# A user class under a builtin exception that has no builtin class id
# (LoadError, NoMemoryError, SystemStackError, SystemCallError, Errno::*,
# IO::EAGAINWaitReadable, ...) answered Object for its superclass, so
# #ancestors, is_a?, Module#< and Module#=== lost the exception chain.
# (CRuby mixes DidYouMean::Correctable into LoadError's ancestors; the lines
# below leave the mixins out of what they print.)

class MyErr < ::LoadError; end
class Deep < MyErr; end
class E2 < ArgumentError; end

p MyErr.ancestors.include?(::LoadError)
p MyErr.ancestors.include?(ScriptError)
p MyErr.ancestors.include?(StandardError)
p MyErr.ancestors.select { |k| k.is_a?(Class) }
p Deep.ancestors.select { |k| k.is_a?(Class) }.first(4)
p E2.ancestors.include?(ArgumentError)

p MyErr.superclass
p MyErr.superclass.superclass
k = Deep
p k.superclass, k.superclass.superclass

e = MyErr.new("x")
p e.is_a?(LoadError), e.kind_of?(ScriptError), e.is_a?(Exception), e.is_a?(StandardError)
p Deep.new("d").is_a?(LoadError), Deep.new("d").kind_of?(ScriptError)

p MyErr < LoadError, MyErr <= ScriptError, MyErr < Exception, MyErr < StandardError
p LoadError > MyErr, ScriptError >= Deep, MyErr <=> LoadError, LoadError <=> MyErr
p MyErr <=> StandardError, LoadError < SystemCallError

p LoadError === e, ScriptError === e, Exception === e, StandardError === e

begin
  raise MyErr, "m"
rescue StandardError
  p :standard
rescue ScriptError => x
  p [:script, x.class, x.message]
end

case Deep.new("d")
when StandardError then p :std
when LoadError then p :load
end

class N < NoMemoryError; end
class SS < SystemStackError; end
p N.superclass, N.superclass.superclass, N < Exception, N < StandardError
p SS.ancestors.include?(Exception), SS.new.is_a?(SystemStackError), Exception === SS.new

class SC < SystemCallError; end
p SC.superclass, SC < StandardError, SC.ancestors.include?(SystemCallError)

class E3 < Errno::ENOENT; end
p E3.superclass, E3.ancestors.first(4), E3 < SystemCallError, E3.new.is_a?(SystemCallError)
begin
  raise E3, "gone"
rescue SystemCallError => x
  p x.class, x.errno
end

class W < IO::EAGAINWaitReadable; end
p W.superclass
p W.ancestors.include?(IO::WaitReadable), W.ancestors.include?(Errno::EAGAIN)
p W < IO::WaitReadable, W < SystemCallError, W.new.is_a?(IO::WaitReadable)

# A superclass written as a path names its builtin exception whole, so the
# superclass table and the `C.superclass` fold carry an exception with a
# builtin id under its path too, not the leaf (DomainError) that named none.
class DE < Math::DomainError; end
class DE2 < DE; end
p DE.superclass, DE.superclass.superclass, DE2.superclass.superclass
p DE2.ancestors.select { |k| k.is_a?(Class) }.first(4)
p DE < StandardError, DE2 < Math::DomainError, Math::DomainError === DE2.new("y")
begin
  raise DE2, "dom"
rescue Math::DomainError => x
  p [x.class, x.message]
end

class CE < Encoding::CompatibilityError; end
class UE < Encoding::UndefinedConversionError; end
p CE.superclass, CE.superclass.superclass, UE < EncodingError, CE.new.is_a?(Encoding::CompatibilityError)
begin
  raise CE, "c"
rescue EncodingError => x
  p x.class
end

# More distinct qualified superclasses than any fixed table would hold: the
# last ones are still exceptions, not Objects.
class X0 < Errno::EPERM; end; class X1 < Errno::ENOENT; end; class X2 < Errno::ESRCH; end
class X3 < Errno::EINTR; end; class X4 < Errno::EIO; end; class X5 < Errno::ENXIO; end
class X6 < Errno::E2BIG; end; class X7 < Errno::ENOEXEC; end; class X8 < Errno::EBADF; end
class X9 < Errno::ECHILD; end; class X10 < Errno::EAGAIN; end; class X11 < Errno::ENOMEM; end
class X12 < Errno::EACCES; end; class X13 < Errno::EFAULT; end; class X14 < Errno::EBUSY; end
class X15 < Errno::EEXIST; end; class X16 < Errno::EXDEV; end; class X17 < Errno::ENODEV; end
class X18 < Errno::ENOTDIR; end; class X19 < Errno::EISDIR; end; class X20 < Errno::EINVAL; end
class X21 < Errno::ENFILE; end; class X22 < Errno::EMFILE; end; class X23 < Errno::ENOTTY; end
class X24 < Errno::EFBIG; end; class X25 < Errno::ENOSPC; end; class X26 < Errno::ESPIPE; end
class X27 < Errno::EROFS; end; class X28 < Errno::EMLINK; end; class X29 < Errno::EPIPE; end
class X30 < Errno::EDOM; end; class X31 < Errno::ERANGE; end; class X32 < Errno::EDEADLK; end
class X33 < Errno::ENAMETOOLONG; end; class X34 < Errno::ENOLCK; end; class X35 < Errno::ENOSYS; end
class X36 < Errno::ENOTEMPTY; end; class X37 < Errno::ELOOP; end; class X38 < Errno::ENOTSOCK; end
class X39 < Errno::EMSGSIZE; end; class X40 < Errno::EPROTOTYPE; end; class X41 < Errno::ENOPROTOOPT; end
class X42 < Errno::EPROTONOSUPPORT; end; class X43 < Errno::ENOTSUP; end; class X44 < Errno::EAFNOSUPPORT; end
class X45 < Errno::EADDRINUSE; end; class X46 < Errno::EADDRNOTAVAIL; end; class X47 < Errno::ENETDOWN; end
class X48 < Errno::ENETUNREACH; end; class X49 < Errno::ENETRESET; end; class X50 < Errno::ECONNABORTED; end
class X51 < Errno::ECONNRESET; end; class X52 < Errno::ENOBUFS; end; class X53 < Errno::EISCONN; end
class X54 < Errno::ENOTCONN; end; class X55 < Errno::ETIMEDOUT; end; class X56 < Errno::ECONNREFUSED; end
class X57 < Errno::EHOSTUNREACH; end; class X58 < Errno::EALREADY; end; class X59 < Errno::EINPROGRESS; end
class X60 < Errno::ESTALE; end; class X61 < Errno::EDQUOT; end; class X62 < Errno::ECANCELED; end
class X63 < Errno::EOVERFLOW; end; class X64 < Errno::EILSEQ; end
p X64.superclass, X64 < SystemCallError, X0.superclass
begin
  raise X64, "late"
rescue SystemCallError => x
  p x.class
end
