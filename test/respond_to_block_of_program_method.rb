# A class's respond_to? asked in a block given to a method of the program's
# answers as of when that method runs the block. A public top-level def is
# an Object method, which a literal's tap, then or yield_self reaches. A
# body that only yields, or calls its block parameter, runs the block
# before it answers, so the question comes before a later def and answers
# false; so does one that tests the block before calling it, or hands it
# on to a method that runs it at once. A body that keeps the block answers
# as of a later run: one that stores the block, hands it on (`&b`, `&`,
# `...`) to a method that stores it, or yields or calls it from a proc, a
# lambda or another method's block. An alias answers as the method it
# names. A lambda called at once runs the block at once, and so does a Proc
# made of the block parameter; a body that runs the block at once and may
# also keep it answers as of the first run. A literal's call in the body
# reaches the version of its method that was live when the body ran.

class S1; end
class S2; end
class S3; end
class S4; end
class S5; end
class S6; end
class S7; end
class S8; end
class S9; end
class S10; end
class S11; end
class S12; end
class S13; end
class S14; end
class S15; end
class S16; end
class S17; end
class S18; end
class S19; end
class S20; end
class S21; end
class S22; end
class S23; end
class S24; end
class S25; end
class S26; end
class S27; end
class S28; end
class S29; end
class S30; end

# runs the block at once
public def tap = (yield self; self)
1.tap { p S1.respond_to?(:z) }
def S1.z = 1
p S1.respond_to?(:z)

public def tap(&b) = (b.call(self); self)
2.tap { p S2.respond_to?(:z) }
def S2.z = 1

$go = true
public def tap
  yield self if $go
  self
end
3.tap { p S3.respond_to?(:z) }
def S3.z = 1

public def tap
  [1].each { yield self }
  self
end
4.tap { p S4.respond_to?(:z) }
def S4.z = 1

def run_now = (yield self; self)
alias tap run_now
public :tap
5.tap { p S5.respond_to?(:z) }
def S5.z = 1

public def then = yield(self)
p("x".then { S6.respond_to?(:z) })
def S6.z = 1

public def yield_self(&b) = b.call(self)
p(:a.yield_self { S7.respond_to?(:z) })
def S7.z = 1

# may keep the block: asked when the kept block runs, after the def
public def tap(&b) = ($b8 = b; self)
8.tap { p S8.respond_to?(:z) }
def S8.z = 1
$b8&.call

def keep(&b) = ($kept = b)
public def tap(...) = (keep(...); self)
9.tap { p S9.respond_to?(:z) }
def S9.z = 1
$kept&.call
$kept = nil

public def tap(&b) = (keep(&b); self)
10.tap { p S10.respond_to?(:z) }
def S10.z = 1
$kept&.call
$kept = nil

public def tap(&) = (keep(&); self)
11.tap { p S11.respond_to?(:z) }
def S11.z = 1
$kept&.call

public def tap
  $q = proc { yield self }
  self
end
12.tap { p S12.respond_to?(:z) }
def S12.z = 1
$q&.call

public def tap(&b)
  $l = -> { b.call(self) }
  self
end
13.tap { p S13.respond_to?(:z) }
def S13.z = 1
$l&.call

def hold(&b) = ($h = b)
public def tap
  hold { yield self }
  self
end
14.tap { p S14.respond_to?(:z) }
def S14.z = 1
$h&.call

public def tap(&b) = ($b15 = b; self)
[1].each { 15.tap { p S15.respond_to?(:z) } }
def S15.z = 1
$b15&.call

def keep_it(&b) = ($b16 = b; self)
alias tap keep_it
public :tap
16.tap { p S16.respond_to?(:z) }
def S16.z = 1
$b16&.call

# runs the block at once, or not at all, after a test of it
public def tap(&b)
  b.call(self) if b
  self
end
17.tap { p S17.respond_to?(:z) }
def S17.z = 1

public def tap(&b)
  return self unless b
  b.to_proc.call(self)
  self
end
18.tap { p S18.respond_to?(:z) }
def S18.z = 1

# hands it on to a method that runs it at once
public def tap(&b)
  instance_exec(self, &b)
  self
end
19.tap { p S19.respond_to?(:z) }
def S19.z = 1

def run_now(x) = yield(x)
public def tap(&b)
  run_now(self, &b)
  self
end
20.tap { p S20.respond_to?(:z) }
def S20.z = 1

public def tap(&b) = super(&b)
21.tap { p S21.respond_to?(:z) }
def S21.z = 1

public def then(&b) = b.curry[self]
22.then { p S22.respond_to?(:z) }
def S22.z = 1

# a helper the call reaches as it was then, not as redefined later
def helper23 = (yield; nil)
public def tap(&b)
  helper23(&b)
  self
end
23.tap { p S23.respond_to?(:z) }
def helper23(&b) = ($b23 = b; nil)
def S23.z = 1

# a literal's own iterator comes before main's method of that name
def each(&b) = ($b24 = b)
public def tap(&b)
  each(&b)
  self
end
[24].tap { p S24.respond_to?(:z) }
def S24.z = 1

# a lambda called at once
public def tap(&b)
  -> { b.call(self) }.call
  self
end
25.tap { p S25.respond_to?(:z) }
def S25.z = 1

# runs it at once, keeps it only on a condition that does not hold
public def tap(&b)
  b.call(self)
  $b26 = b if $never
  self
end
26.tap { p S26.respond_to?(:z) }
def S26.z = 1

# a Proc made of the block parameter, called at once
public def tap(&b)
  Proc.new(&b).call(self)
  self
end
27.tap { p S27.respond_to?(:z) }
def S27.z = 1

public def tap(&b)
  proc(&b).call(self)
  self
end
28.tap { p S28.respond_to?(:z) }
def S28.z = 1

# a literal's call in the body reaches the version that was live when the
# body ran, not the one a later def replaces it with
public def yield_self(&b) = (yield self; self)
public def tap(&b)
  2.yield_self(&b)
  self
end
29.tap { p S29.respond_to?(:z) }
public def yield_self(&b) = ($b29 = b; self)
def S29.z = 1

# ...and where Object answers the callee's name until main's one def, the
# call reaches Object's, which runs the block at once
public def tap(&b)
  30.instance_eval(&b)
  self
end
30.tap { p S30.respond_to?(:z) }
public def instance_eval(&b) = ($b30 = b; self)
def S30.z = 1
