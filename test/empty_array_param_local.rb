# Issue #58: an empty `[]` literal stored in a local and then passed
# to a method whose body pushes a known-type element should compile
# cleanly. Both ends — the caller's local and the callee's parameter —
# need to converge to the same concrete typed-array type.

def collect_names(buf)
  buf.push("alpha")
  buf.push("beta")
end

names = []
collect_names(names)
puts names[0]      # alpha
puts names[1]      # beta
puts names.length  # 2
