# Polymorphic dispatch where the called method dereferences `self`.
# `cls_id` used to share memory with `v.p` inside the sp_RbVal union,
# so writing cls_id after v.p clobbered the low 32 bits of the pointer.
# Methods that touch instance fields then crashed (SIGSEGV).
# Methods that did not touch self happened to work, masking the bug.

class C
  attr_accessor :n
  def initialize(n)
    @n = n
  end
  def name
    @n.to_s
  end
end

class D
  def name
    "other"
  end
end

def show(o)
  o.name
end

puts show(C.new(42))
puts show(D.new)
