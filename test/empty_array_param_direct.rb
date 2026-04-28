# Issue #58: an `[]` literal passed directly at the call site must
# also see the deferred-element-type resolution, not just one stored
# in a local first.

def push_floats(buf)
  buf.push(1.5)
  buf.push(2.5)
  buf
end

result = push_floats([])
puts result[0]   # 1.5
puts result[1]   # 2.5
