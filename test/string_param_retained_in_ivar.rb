# A string handed to a method and RETAINED in an ivar that is mutated in place
# has to stay the caller's string. The byref machinery only covered a callee
# that mutates the PARAMETER itself; one that merely stores it and mutates it
# later, through the ivar, left the caller holding a value copy and the two
# drifted apart (#4363). Both directions matter: the callee's mutation must
# reach the caller's name, and the caller's must reach the ivar.

class Holder
  attr_reader :bt
  def initialize(bt)
    @bt = bt
  end
  def bump
    @bt << "!"
  end
end

a = +"aaa"
h = Holder.new(a)
h.bump
p [h.bt, a]
a << "?"
p [h.bt, a]

# the same through a plain method rather than the constructor
class Sink
  attr_reader :s
  def initialize
    @s = +""
  end
  def take(x)
    @s = x
  end
  def bump
    @s << "!"
  end
end

b = +"bbb"
k = Sink.new
k.take(b)
k.bump
p [k.s, b]
b << "?"
p [k.s, b]

# a fresh string with no caller alias still works, and stays independent
g = Holder.new(+"ggg")
g.bump
p g.bt

# two holders over the same string see each other
c = +"ccc"
h1 = Holder.new(c)
h2 = Holder.new(c)
h1.bump
p [h1.bt, h2.bt, c]

# The mutators whose names are Array's and Hash's as much as String's --
# `[]=`, `insert`, `slice!`, `setbyte` -- are in-place once the slot's type
# says String, and the shim emits them on the handle. Statement position and
# expression position (a method's last expression) go through different
# emitters, so both are exercised.
class Edits
  attr_reader :bt
  def initialize(bt)
    @bt = bt
  end
  def splice
    @bt[0, 3] = "XYZ"
    nil
  end
  def ins
    @bt.insert(1, "-")
    nil
  end
  def sl
    @bt.slice!(0, 1)
    nil
  end
  def sb
    @bt.setbyte(0, 90)
    nil
  end
  def splice_tail
    @bt[0, 1] = "q"
  end
  def ins_tail
    @bt.insert(0, "w")
  end
end

d = +"aaaaaa"
e = Edits.new(d)
e.splice
p [e.bt, d]
e.ins
p [e.bt, d]
e.sl
p [e.bt, d]
e.sb
p [e.bt, d]
e.splice_tail
p [e.bt, d]
e.ins_tail
p [e.bt, d]
d << "="
p [e.bt, d]

# a reader in ARGUMENT position hands out the handle, not a safe copy, so the
# receiving holder and the source object keep one string between them
class Src
  attr_reader :bytes
  def initialize
    @bytes = +"src"
  end
end

s = Src.new
r = Holder.new(s.bytes)
r.bump
p [r.bt, s.bytes]

# The chain carries however deep it runs: a parameter passed on through
# another method before it is stored has to arrive as the handle too, or the
# name the outermost caller holds goes its own way.
class Keep
  attr_reader :s
  def initialize
    @s = +""
  end
  def hold(x)      # a name of its own: the callee is resolved by unique name,
    @s = x         # and `take` above already claims that one
  end
  def bump
    @s << "!"
  end
end

def hand_on(k, y)
  k.hold(y)
end

t = +"ttt"
kk = Keep.new
hand_on(kk, t)
kk.bump
p [kk.s, t]
t << "?"
p [kk.s, t]

# A slot mutated only from OUTSIDE its class records nothing in the mutation
# census, which keys on calls whose receiver is an ivar read. Being a shared
# handle already is evidence enough for the parameter written into it.
class Outside
  attr_reader :s
  def initialize(x)
    @s = x
  end
end

o = +"ooo"
w = Outside.new(o)
w.s << "!"
p [w.s, o]
o << "?"
p [w.s, o]

# Identity, not only visibility: two names for one shared string answer
# equal? true, read through a reader on either side. The mark that makes a
# reader hand out the handle is also how the node's type is read, and the
# emitters pick their arm from the receiver's type -- so the demand travels in
# its own array, and the receiver still dispatches as the String it is.
i = +"iii"
h3 = Holder.new(i)
h3.bump
p [h3.bt.equal?(i), i.equal?(h3.bt), h3.bt.equal?(h3.bt)]

# One parameter stored into TWO ivars where only one is mutated. The mutated
# slot becomes a handle through its own evidence; the other has none of its
# own, and what a handle is assigned to is a handle.
class Pair
  attr_reader :one, :two
  def initialize(s)
    @one = s
    @two = s
  end
  def bump
    @one << "!"
  end
end

q = +"qqq"
pr = Pair.new(q)
pr.bump
p [pr.one, pr.two, q]
p [pr.one.equal?(q), pr.one.equal?(pr.two)]
q << "?"
p [pr.one, pr.two]

# Neither slot has evidence of its own. The RETAINING class never mutates what
# it holds, and the slot the mutation goes through belongs to a DIFFERENT
# class, so the mutation census -- which keys on calls whose receiver is an
# ivar read -- records nothing against the retaining one. The propagation only
# ever ran param -> argument (a parameter that is byref, or already a handle,
# pulls its call sites' locals into the shared set), so the reading half was
# handed sp_str_concat(...) -- a snapshot -- and never saw the write. What a
# handle is PASSED TO is a handle, which is the call-site twin of the rule the
# Pair case above covers.
class Peek
  def initialize(b)
    @b = b
  end
  def at(i)
    @b.getbyte(i)
  end
end

class Poke
  def initialize(b)
    @b = b
  end
  def poke(i, v)
    @b.setbyte(i, v)
  end
end

z = +"abcd"
pe = Peek.new(z)
po = Poke.new(z)
po.poke(2, 7)
p [pe.at(2), z.getbyte(2)]

# The same with the handle reaching the argument through a READER rather than a
# name, and with the buffer built by setbyte on a local before it is handed on.
class Box
  attr_reader :bytes
  def initialize(bytes)
    @bytes = bytes
  end
end

def built(n)
  buf = Array.new(n, 0).pack("C*")
  i = 0
  while i < n
    buf.setbyte(i, 1)
    i += 1
  end
  buf
end

bx = Box.new(built(4))
pe2 = Peek.new(bx.bytes)
po2 = Poke.new(bx.bytes)
po2.poke(1, 9)
p [pe2.at(1), bx.bytes.getbyte(1)]

# A scope named `new` that is never called by that name. The call-site index
# behind these rules resolves `K.new(...)` two ways -- a method matching the
# call's name, and a constant receiver's `initialize` -- and both answers are
# live at once here: `Holder.new(s)` matches this method BY NAME and is the
# constructor of Holder. Keeping only one of them dropped the call from the
# other scope's call sites, and with it the evidence that the argument is a
# handle, so the pair below went back to a copy.
class Factory
  def new(x)      # never called: it only has to EXIST for the name arm to
    x + 1         # resolve. Calling it as `Factory.new.new(1)` would put a
  end             # non-constant receiver on a `new` call, and the whole-program
end               # guard would switch constructor resolution off instead.

zz = +"abcd"
pe3 = Peek.new(zz)
po3 = Poke.new(zz)
po3.poke(3, 5)
p [pe3.at(3), zz.getbyte(3)]

# A `new` whose receiver is not a constant makes the class it constructs
# unpinnable: codegen emits a switch over the receiver's class id, with an arm
# for every class whose initialize accepts the call's shape, and each arm passes
# the argument in that parameter's own C type. A class the switch can land on
# has to keep the type the arm was written against, so its parameter is left
# alone.
#
# That question used to be asked of the whole PROGRAM -- one such `new`
# anywhere switched constructor resolution off for every class at once. A
# no-argument `k.new` cannot construct a class whose initialize requires one,
# so an unrelated dynamic `new` in an unrelated file silently cost every
# retained string in the program its handle, and brought the copy back.
class Unrelated
  def initialize
    @n = 1
  end
  def n
    @n
  end
end

def pick(c)
  c
end

u = +"abcd"
pu = Peek.new(u)
qu = Poke.new(u)
qu.poke(0, 8)
p [pu.at(0), u.getbyte(0)]
p pick(Unrelated).new.n          # a `new` on a receiver that is not a constant
