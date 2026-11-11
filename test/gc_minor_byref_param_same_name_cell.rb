# A method that appends to its String parameter appends to the caller's
# String. The callee once stored through `const char **_cell_io`, which the
# caller could point at its own stack local (`emit(&lv_io)`); such a store
# took no barrier -- the lending site recorded a heap cell it lent, and the
# caller's frame rooted a stack local.
# spinel: gc-minor
def emit(io)
  io << "a"
  io << "b"
end

def run
  io = +""
  emit(io)
  io
end

# An unrelated local of the SAME NAME, captured by a proc, is a heap cell. The
# cell-barrier scan collects cell names over the whole program, so before the
# fix this one made every `(*_cell_io) = v` in `emit` take `sp_gc_wb`, which
# read a GC header off the caller's stack slot and, when the bytes in front of
# it read as an old clean header, set a dirty bit in the caller's frame.
def other
  io = +""
  add = proc { |x| io << x.to_s }
  add.call(1)
  add.call(2)
  io
end

puts run
puts other
