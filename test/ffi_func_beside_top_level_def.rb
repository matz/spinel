# An ffi_func is its module's singleton method: `M.name` with an explicit
# receiver reaches it even when the program also has a top-level def of the
# same name (Object's private method). The explicit-receiver refusal for a
# private top-level def raised NoMethodError here ("private method
# 'board_name' called for module Board"), which is the shape an embedded
# board layer takes: a Ruby helper wrapping the C function of its name.
def board_name
  Board.board_name.delete("\0") + "!"
end

module Board
  ffi_source <<~C
    const char *board_name(void) { return "Board X"; }
    int board_led(int on) { return on ? 1 : 0; }
  C
  ffi_func :board_name, [], :str
  ffi_func :board_led, [:int], :int
end

def board_led(on) = Board.board_led(on) + 10

puts board_name
puts Board.board_name
puts board_led(1)
puts Board.board_led(0)
