# A String reached through a reader or a container element keeps its identity
# when it is passed to a method that mutates it in place. #5112 made the ivar
# a shared handle when the mutation goes THROUGH the reader; the mutation the
# CALLEE performs on what the reader HANDED OUT stayed a copy, and the
# caller's string came back empty with no error.
#
# Each source shape gets its own helper: a plain local and a reader result each
# reach the callee as a handle.
def push_reader(x);    x << "!"; end
def push_alias(x);     x << "!"; end
def push_getter(x);    x << "!"; end
def push_hash(x);      x << "!"; end
def push_array(x);     x << "!"; end
def push_local(x);     x << "!"; end
def push_ivar(x);      x << "!"; end

class Box
  attr_reader :reader
  def initialize; @reader = +""; end
  def getter; @reader; end
end

b = Box.new; push_reader(b.reader);            p b.reader
b = Box.new; t = b.reader; push_alias(t);      p b.reader
b = Box.new; push_getter(b.getter);            p b.reader

h = { k: +"" }; push_hash(h[:k]);              p h[:k]
a = [+""];      push_array(a[0]);              p a[0]

# the shapes that already worked: a plain local, and a bare ivar argument --
# both lvalues the caller can hand over as a handle
s = +"a"; push_local(s);                       p s

class Inner
  def initialize; @buf = +""; end
  def go; push_ivar(@buf); @buf; end
end
p Inner.new.go

# one helper reached with BOTH a reader argument and a plain local: the
# parameter takes the handle and the lvalue is normalized into it, so both
# mutations land. Either order.
def push_both(x); x << "!"; end
bb = Box.new; push_both(bb.reader); ss = +"a"; push_both(ss)
p bb.reader
p ss

def push_both2(x); x << "!"; end
tt = +"a"; push_both2(tt); cc = Box.new; push_both2(cc.reader)
p tt
p cc.reader

# one helper reached with a reader argument AND a container element: two
# argument shapes that each demand the handle, through one parameter. The
# reader's handle used to be wrapped in a fresh one (sp_String_new_shared
# takes a value, not a handle) and the C build stopped.
def push_mixed(x); x << "!"; end
mb = Box.new; push_mixed(mb.reader)
mh = { k: +"" }; push_mixed(mh[:k])
ma = [+""];      push_mixed(ma[0])
ml = +"a";       push_mixed(ml)
p mb.reader
p mh[:k]
p ma[0]
p ml
