# each_line and lines with their arguments handed on in a splat (a wrapper
# that forwards `*args, &block`): an empty splat splits at newlines, one
# element is the separator, and a nil one keeps the String whole.
def lines_of(s, *args, &block)
  s.each_line(*args, &block)
end

def list_of(s, *args)
  s.lines(*args)
end

out = []
lines_of("x\ny") { |l| out << l }
lines_of("a|b", "|") { |l| out << l }
lines_of("p\nq", nil) { |l| out << l }
p out
p list_of("x\ny"), list_of("a|b", "|"), list_of("p\nq", nil)
p lines_of("m\nn").class
