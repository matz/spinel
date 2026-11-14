# Array#fill(start) { } compiles when start is a proc call that can return nil
# For b.fill(st.call(3)) { }, the generated C placed the setup statements of
# st.call(3) in the middle of a variable declaration, and the C did not build.
st = ->(q) { q > 2 ? 1 : nil }
b = [1, 2, 3, 4]
p b.fill(st.call(3)) { |i| i * 3 }
p b.fill(st.call(1)) { |i| i * 2 }
p b.fill(0, st.call(3)) { |i| i * 3 }
p b.fill(1, st.call(1)) { |i| i + 1 }
cnt = 0
tr = ->(q) { cnt += 1; q }
p b.fill(tr.call(2)) { |i| i + 10 }, cnt
p b.fill(tr.call(nil)) { |i| i + 20 }, cnt
