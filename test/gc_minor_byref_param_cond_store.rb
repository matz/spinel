# A method that appended to its String parameter once took it by reference
# (`const char **_cell_io`, pointing at the caller's stack local), and such
# a store took no barrier. Here a condition that assigns through `||`
# breaks its C over lines, one of which starts at column 0 and ends with
# `{`. Taken as the function's header, it hid `_cell_io` from the check,
# the stores after it took `sp_gc_wb` on the caller's stack slot, and a
# minor collection called whatever stale return address sat in front of it
# as a scan function (#9197, campfire's layout).
# spinel: gc-minor
class Flash
  def initialize(h) = @h = h
  def [](k) = @h[k]
end

def layout_into(io, flash)
  io << "<a>"
  if notice = flash[:notice] || flash[:alert]
    io << notice
  end
  io << "</a>"
end

def layout(flash)
  io = +""
  layout_into(io, flash)
  io
end

# a heap cell of the same name elsewhere puts `io` among the cells the
# barrier scan looks for
def other
  io = +""
  add = proc { |x| io << x.to_s }
  add.call(1)
  io
end

puts layout(Flash.new({ notice: "hi" }))
puts layout(Flash.new({ alert: "no" }))
puts layout(Flash.new({}))
puts other
