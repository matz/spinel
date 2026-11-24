# The exception-frame stack is fixed, and the depth that fills it is dynamic,
# not lexical: this method's begin frame stays armed across the recursive
# call it makes, so a recursion with no end arms one frame per level until the
# array is full. That is a SystemStackError raised into the frame on top
# (test/handler_stack_overflow.rb rescues it); here nothing rescues it, so
# the program ends with CRuby's words on stderr and exit 1, as CRuby's does
# when its own, far deeper, stack runs out. Neither prints anything, which is
# what lets the suite hold this: the CRuby-generated .expected is empty and
# stays empty (CRuby's non-zero exit keeps regen off), and the .err.expected
# sidecar pins the message.
def deep_begin(n)
  begin
    deep_begin(n + 1) + 1
  rescue ZeroDivisionError
    -1
  end
end
deep_begin(0)
puts "unreachable"
