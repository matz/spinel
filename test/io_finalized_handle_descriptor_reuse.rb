# A handle the collector finalizes without a close leaves nothing behind for
# the next handle that gets its descriptor number: a reader on the new one
# still wakes when a line arrives.
ENV["SPINEL_WORKERS"] = "1"

def read_once
  r, w = IO.pipe
  t = Thread.new { r.gets }
  sleep 0.05
  w.puts "x"
  t.join
  nil
end

read_once
GC.start
r, w = IO.pipe
t = Thread.new { r.gets }
sleep 0.05
w.puts "y"
puts(t.join(2) ? "reader woke" : "reader stuck")
