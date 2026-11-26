# IO#puts and Kernel#warn wait for a full pipe as IO#write does: four
# threads write more than a pipe buffer into an IO.pipe, and into $stderr
# reopened onto a pipe or assigned one, while a reader thread drains it,
# with two workers.
ENV["SPINEL_WORKERS"] = "2"   # one helper, under any outside setting

def bad_lines(text)
  text.lines.count { |l| l !~ /\At\d line \d+\n\z/ }
end

# 4 threads x 1,500 lines of at most 14 bytes: more than a 64 KiB pipe holds
def writers(lines = 1500)
  (0...4).map { |n| Thread.new(n) { |t| lines.times { |i| yield t, i } } }.each { |th| th.join(20) }
end

r, w = IO.pipe
reader = Thread.new { r.read }
writers { |t, i| w.puts "t#{t} line #{i}" }
w.close
text = reader.join(20) ? reader.value : ""
p [text.lines.size, bad_lines(text)]

def through_stderr
  r, w = IO.pipe
  reader = Thread.new { r.read }
  saved = $stderr.dup
  $stderr.reopen(w)
  w.close
  yield
  $stderr.reopen(saved)
  reader.join(20) ? reader.value : ""
end

text = through_stderr { writers { |t, i| $stderr.puts "t#{t} line #{i}" } }
p [text.lines.size, bad_lines(text)]

# CRuby drops some warn lines written through a reopened $stderr, so its
# count is a lower bound; Spinel writes each one
def all_warned(text, lines = 6000)
  RUBY_ENGINE == "spinel" ? text.lines.size == lines : text.lines.size >= 1000
end

text = through_stderr { writers { |t, i| warn "t#{t} line #{i}" } }
p [all_warned(text), bad_lines(text)]

r, w = IO.pipe
reader = Thread.new { r.read }
saved = $stderr
$stderr = w
# 60,000 lines: a pipe's buffer can grow on its own past a few thousand
writers(15000) { |t, i| warn "t#{t} line #{i}" }
$stderr = saved
w.close
text = reader.join(20) ? reader.value : ""
p [all_warned(text, 60000), bad_lines(text)]

# putc and print after puts, while the pipe is full: each byte arrives
r, w = IO.pipe
reader = Thread.new { sleep 0.2; r.read }
w.puts "start"
70000.times { w.putc "x" }
20000.times { |i| w.print "p#{i} " }
w.close
text = reader.join(20) ? reader.value : ""
p [text.bytesize, text.count("x"), text.end_with?("p19999 ")]

# the same after `sync = false`, which leaves the descriptor non-blocking
r, w = IO.pipe
reader = Thread.new { sleep 0.2; r.read }
w.puts "seed"
w.sync = false
70000.times { w.putc "x" }
w.puts "end"
w.close
text = reader.join(20) ? reader.value : ""
p [text.bytesize, text.count("x"), text.end_with?("end\n")]
