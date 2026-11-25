# The first index of a run of element updates behind one joint guard is
# computed ahead of the run: an error it raises still names its own line,
# not the statement's before it (backtrace-test).
def upd(h, z, n)
  i = 0
  while i < n
    q = i
    h[q / z] += 1.0
    h[i + 1] += 2.0
    i += 1
  end
end

h = [0.0, 0.0, 0.0]
upd(h, 1, 2)
begin
  upd(h, 0, 1)
rescue ZeroDivisionError => e
  puts e.backtrace.first
end
