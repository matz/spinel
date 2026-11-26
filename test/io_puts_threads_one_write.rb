# IO#puts and Kernel#warn from several threads write each line with its
# newline, on a sync File, a pipe, and $stderr and $stdout reopened onto a
# pipe, so no line of one thread lands between another thread's text and
# newline.
ENV["SPINEL_WORKERS"] ||= "4"
require "tmpdir"

def bad_lines(text)
  text.lines.count { |l| l !~ /\At\d line \d+\n\z/ }
end

# 500 lines of at most 12 bytes (about 5.5 KB) stay under a pipe's smallest
# buffer (8 KiB, two pages for a user over fs.pipe-user-pages-soft), so no
# writer waits for the reader thread to drain the pipe.
def spam(io)
  (0...4).map { |n| Thread.new(n) { |t| 125.times { |i| io.puts "t#{t} line #{i}" } } }.each { |th| th.join(30) }
end

def spam_warn
  (0...4).map { |n| Thread.new(n) { |t| 125.times { |i| warn "t#{t} line #{i}" } } }.each { |th| th.join(30) }
end

def through_pipe(std)
  r, w = IO.pipe
  reader = Thread.new { r.read }
  saved = std.dup
  std.reopen(w)
  w.close
  yield
  std.reopen(saved)
  reader.value
end

path = File.join(Dir.tmpdir, "spinel_puts_threads_#{Process.pid}.txt")
f = File.open(path, "w")
f.sync = true
spam(f)
f.close
text = File.read(path)
File.delete(path)
p [text.lines.size, bad_lines(text)]

r, w = IO.pipe
reader = Thread.new { r.read }
spam(w)
w.close
text = reader.value
p [text.lines.size, bad_lines(text)]

text = through_pipe($stderr) { spam($stderr) }
p [text.lines.size, bad_lines(text)]

text = through_pipe($stdout) { spam($stdout) }
p [text.lines.size, bad_lines(text)]

# CRuby 4.0 drops some of the warn lines written through the reopened
# $stderr, so the count is a lower bound
text = through_pipe($stderr) { spam_warn }
p [text.lines.size >= 250, bad_lines(text)]
