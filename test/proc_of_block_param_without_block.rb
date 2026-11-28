# A method that turns its &b into a proc (`proc(&b)`), called without a
# block: the param is nil there, so the call is CRuby's ArgumentError, where
# the spliced body named a block param no context held (the C did not
# compile). A call with a block answers that block's proc.
def as_proc(&b) = proc(&b)
def kept(&b)
  x = proc(&b)
  x
end
as_proc() if ARGV.size > 99
p as_proc { |a| a }.call(1)
p kept { |a| a + 1 }.call(1)
p((as_proc rescue $!.message))
p((kept rescue $!.message))
