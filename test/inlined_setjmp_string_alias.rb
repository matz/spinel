# Follow-up to #6552: a rebound String parameter has both a private slot
# and a selector pointing at it. Both must survive break's longjmp.
def alias_guard(io, prebind)
  io << "["
  io = String.new("pre") if prebind
  begin
    io = String.new("fresh")
    yield
    0
  ensure
    puts io
  end
end

first = String.new("caller")
p alias_guard(first, false) { break 79 }
puts first
second = String.new("caller")
p alias_guard(second, true) { break 89 }
puts second

# The inner expansion borrows the outer expansion's qualified private slot.
def alias_outer(io)
  io << "{"
  begin
    io = String.new("outer")
    alias_guard(io, false) { yield }
    0
  ensure
    puts io
  end
end
third = String.new("caller")
p alias_outer(third) { break 97 }
puts third

# An ordinary appending method must retain the borrowed slot's qualifier.
def append_one(s)
  s << "!"
end
def borrowed_guard(io)
  io << "{"
  begin
    io = String.new("borrow")
    yield
    0
  ensure
    append_one(io)
    puts io
  end
end
fourth = String.new("caller")
p borrowed_guard(fourth) { break 101 }
puts fourth
