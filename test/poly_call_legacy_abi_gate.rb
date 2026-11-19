# spinel: not-cruby -- a target the legacy cast cannot reach declines with
# NoMethodError, and an argument of the wrong pointer kind raises TypeError,
# where CRuby makes the call.
# Regression: a boxed bound Method read out of a poly slot may only be invoked
# through the legacy `sp_int (*)(void *, sp_int...)` cast when the call-site
# argument class matches the target's stamped per-position C ABI type. The bind
# sites stamp method_legacy_int_abi as 1 (callable) with a per-position type
# signature, or 0 (decline), and the poly `.call`/`[]` paths compare each
# position's type against it. A target that cannot ride the cast -- a
# float return, a keyword parameter, a rest parameter, an argument count
# that does not match the signature, or a pointer of the wrong kind (a
# String parameter fed an IntArray, or an object of another class) -- falls
# through to NoMethodError instead of reading (or being read as) the wrong
# C type: garbage or a segfault. A class method that needs its receiving
# class runs through its Method's thunk, which passes the class.
#
# A parameter nothing typed is no longer one of those cases. It used to be
# pinned to `sp_int`, so a String handed to it would have been read as an
# integer and the gate refused the call; it is poly now, which is what the
# bound-Method ABI carries anyway, so the call goes through and answers what
# CRuby answers (`ptr_to_untyped` below).
#
# A pointer-typed target parameter and a matching pointer argument DO ride the
# cast (the pointer is laundered through the sp_int slot), so that keeps
# working; a mixed int+pointer list works when each position's type matches; an
# optional/defaulted parameter works at FULL arity (the C signature reads every
# fixed slot, so a shorter call declines); a rest parameter ALWAYS declines.
# The callee signature has an extra trailing sp_PolyArray* the cast cannot
# supply, and the callee prologue roots that parameter unconditionally
# (SP_GC_ROOT(lv_<rest>)), so the garbage register the cast left there is
# handed to the collector on the next allocation -- a segfault. Supplying a
# real rest array would need a run-time dispatch on the target's fixed arity,
# which the static cast cannot do, so even an unread rest is declined.
#
# This file defines a user class `call` (Handler) on purpose, so the SHADOWED
# pre-arm serves the `.call` cases; poly_call_fast_abi_gate.rb covers the
# no-user-`call` fast path.
#
# The snapshot is hand-written. Since #4542 a target the stamped ABI cannot
# take rides the per-target thunk its bind site stamped, so most of the
# former declines answer as CRuby does; what still diverges is the thunk's
# boundary check (an argument of another kind than the compiled parameter is
# a TypeError here, where CRuby runs the body with it), a class method that
# takes its class, a bound builtin's wrapper, and the 16-slot cap.

class Handler
  def call(x) = x
end

def expect_nome(label)
  yield
  puts "#{label}: no raise"
rescue NoMethodError
  puts "#{label}: NoMethodError"
rescue TypeError => e
  # the thunk's boundary check: an argument of another kind than the
  # compiled parameter (#4542)
  puts "#{label}: TypeError: #{e.message}"
rescue ArgumentError => e
  # a count the target's signature cannot bind: the thunk's ArgumentError,
  # in CRuby's words (#4542)
  puts "#{label}: ArgumentError: #{e.message}"
end

class Base
  def int_method(x) = x + 1
  def bool_method(x) = x > 0
  def sym_method(x) = :sym
  def opt_method(a, b = 10) = a + b
  def rest_used(a, *r) = a + r.length
  # A crash regression: an allocating rest body whose bogus rest pointer the
  # prologue would root. The gate must decline before the body runs; if it is
  # ever loosened again the allocation loop forces a collection and the
  # garbage rest pointer reaches sp_gc_mark (SIGSEGV).
  def rest_unused(a, *r)
    x = "abc"
    i = 0
    while i < 100_000
      y = [x, x, x]
      i += y.length
    end
    a
  end
  def rest_anon(a, *) = a
  def kw_method(x: 5) = x
  def two_args(a, b) = a + b
  def str_method(s) = s.length
  def iarr_method(a) = a.length
  def untyped_method(s) = 42
  def self.cmethod(a) = self.name.length + a
end

class Derived < Base; end
# (named, so the program keeps it: a class nothing names is left out)
DERIVED = Derived

# A bool or Symbol return rides the cast: the bind site stamps the kind and
# the box reads the low byte / the Symbol id (poly_method_call_return_kinds.rb
# has every kind).
p [Base.new.method(:bool_method)][0].call(1)
p [Base.new.method(:sym_method)][0].call(1)
expect_nome("optional")      { [Base.new.method(:opt_method)][0].call(1) }
expect_nome("rest_used")     { [Base.new.method(:rest_used)][0].call(1, 2, 3) }
# An unused rest parameter is declined too: the callee prologue roots its
# (garbage) rest slot unconditionally, and any allocation in the body would
# hand that pointer to the collector.
expect_nome("rest_unused")   { [Base.new.method(:rest_unused)][0].call(1, 2, 3) }
expect_nome("rest_anon")     { [Base.new.method(:rest_anon)][0].call(1, 2, 3) }
expect_nome("keyword")       { [Base.new.method(:kw_method)][0].call }
expect_nome("arity")         { [Base.new.method(:two_args)][0].call(1) }
expect_nome("cmethod")       { [Base.method(:cmethod)][0].call(5) }
p [Base.method(:cmethod)][0].call(5)   # (the thunk passes Base: 4 + 5)
expect_nome("slice")         { [Base.new.method(:rest_used)][0][1, 2] }
expect_nome("ptr_to_untyped"){ [Base.new.method(:untyped_method)][0].call("hello") }
# The pointer positions must agree by type, not merely by pointer-ness.
expect_nome("ptr_mismatch")  { [Base.new.method(:str_method)][0].call([1, 2, 3]) }
expect_nome("ptr_mismatch2") { [Base.new.method(:iarr_method)][0].call("hello") }
# An UnboundMethod is not callable, even read out of a container.
um = Base.new.method(:int_method).unbind
umslot = [um]
expect_nome("unbound_call")  { umslot[0].call(1) }
expect_nome("unbound_aref")  { umslot[0][1] }

# Seed str_method's parameter so the emitted signature is sp_String *; the
# pointer argument then rides the sp_int register intact.
Base.new.str_method("seed")
Base.new.iarr_method([1, 2, 3])
puts [Base.new.method(:str_method)][0].call("hello")
puts [Base.new.method(:iarr_method)][0].call([1, 2, 3, 4])
# An optional parameter is callable at full arity (the short form declines).
puts [Base.new.method(:opt_method)][0].call(1, 2)
puts [Base.new.method(:opt_method)][0][3, 4]
# In the shadowed pre-arm a typed-array adapter's Ruby return is boxed by its
# recorded kind (push answers the array, not a mis-tagged Integer).
sa = ["x"]
puts [sa.method(:push), Handler.new][0].call("y").inspect
puts sa.inspect

# A StrArray `[]` is served by its synthesized __bam_ wrapper, whose String
# answer is laundered through the sp_int register; the wrapper's stamped
# legacy_ret boxes it as a String (an out-of-range read answers nil).
sag = ["x", "z"]
puts [sag.method(:[]), Handler.new][0].call(0)
puts [sag.method(:[]), Handler.new][0].call(9).inspect

# Method#to_proc on a Method read out of a poly slot wraps the generic
# sp_method_proc_tramp. It has no call-site types, so it declines a target
# that cannot ride the legacy ABI instead of forwarding the raw cast -- a
# rest target's garbage pointer reached SP_GC_ROOT/sp_gc_mark and segfaulted.
expect_nome("toproc_rest")      { [Base.new.method(:rest_unused)][0].to_proc.call(1, 2, 3) }
expect_nome("toproc_rest_anon") { [Base.new.method(:rest_anon)][0].to_proc.call(1, 2, 3) }
p [Base.new.method(:bool_method)][0].to_proc.call(0)
expect_nome("toproc_arity")     { [Base.new.method(:two_args)][0].to_proc.call(1) }
# A pointer-typed parameter is declined on this generic route too: it has no
# call-site types, so an Integer argument would be read as a raw address
# (sp_str_length(1) segfaulted). The .call route checks the classes position
# by position and can still accept a matching pointer; the trampoline cannot.
expect_nome("toproc_ptr")       { [Base.new.method(:str_method)][0].to_proc.call(1) }
# A callable target still rides the trampoline.
puts [Base.new.method(:int_method)][0].to_proc.call(4)

# A receiverless Kernel wrapper (`method(:String)`) is bound with self NULL,
# so its __bam_r is a REAL argument: the poly pre-arm must pass it (fixed=1)
# rather than treat it as the self slot. Before the receiver-bound flag it
# stamped fixed=0, so call(123) declined and call() invoked the wrapper
# through an undefined register (printed "0", UB).
puts [method(:String)][0].call(123).inspect
begin
  [method(:String)][0].call
  puts "kernel_zero: no raise"
rescue => e
  # CRuby raises ArgumentError here; the poly gate's documented mismatch
  # answer is NoMethodError (the same class it uses for every other
  # arity/type mismatch). No undefined register is read.
  puts "kernel_zero: #{e.class}"
end

# A top-level method has no self parameter. The generic trampoline must use
# the self-less cast or every argument shifts by one: this answered 1
# (fn(NULL, 1, 2) on `def top_add(a, b) = a + b`) instead of 3.
def top_add(a, b) = a + b
puts [method(:top_add)][0].to_proc.call(1, 2)
