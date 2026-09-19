# An unresolved call on a boxed receiver emits the nomethod gate's raise
# token, an sp_RbVal. As an int-arithmetic OPERAND that token must be
# discarded-and-coerced (the raise means its value is never read), not passed
# raw into the sp_int slot -- `io.stat.size - io.pos` on a boxed handle did
# not build. At run time the raise carries the usual NoMethodError, so the
# rescue-modifier form answers its fallback, as CRuby does.
h = {a: $stdin, b: "s"}
io = h[:b]
n = (io.stat.size - io.pos rescue 7)
p n
begin
  p(io.stat.size - io.pos)
rescue NoMethodError
  puts "nomethod"
end
