# A POLY receiver reaches a method through the cls_id switch, and that switch
# hoists its arguments once -- by the argument's own type, for every arm to
# share. It has no callee to ask, so a name a poly receiver can reach once
# kept the value ABI, and every arm appended to a copy: this printed "p". Each
# arm now takes the caller's handle, and the answer is CRuby's.
def frag_into(io, n)
  io << "f#{n};"
  nil
end

class A
  def go(b)
    frag_into(b, 1)
  end
end

class B
  def go(b)
    frag_into(b, 2)
  end
end

list = [A.new, B.new]
buf = String.new("p")
list.each { |o| o.go(buf) }
p buf

# a statically resolved call into the same method appends to the caller's too
def wrap_into(io)
  frag_into(io, 9)
  nil
end
direct = String.new("d")
wrap_into(direct)
p direct
