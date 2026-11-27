# A standard stream reopened onto an IO.pipe write end takes the pipe's
# sync, as CRuby's reopen takes the other IO's mode: it writes through,
# and a write into a full pipe waits for the reader thread to drain it
# instead of sleeping in write(2) on the worker that reader also needs.
# Past a pipe's buffer (64 KiB on Linux, smaller on macOS) on one worker,
# the writer used to hang.
ENV["SPINEL_WORKERS"] ||= "1"

def through_pipe(std)
  r, w = IO.pipe
  reader = Thread.new { r.read }
  saved = std.dup
  std.reopen(w)
  w.close
  sync = std.sync
  yield
  std.reopen(saved)
  [reader.value, sync, std.sync]
end

line = "x" * 60
text, during, after = through_pipe($stdout) { 2000.times { |i| $stdout.puts "#{i} #{line}" } }
p [text.lines.size, text.bytesize > 65536, during, after]

text, during, after = through_pipe($stderr) do
  (0...4).map { |n| Thread.new(n) { |t| 500.times { |i| $stderr.puts "t#{t} #{i} #{line}" } } }.each(&:join)
end
p [text.lines.size, during, after]

f = File.open(File::NULL, "w")
saved = $stdout.dup
$stdout.reopen(f)
on_file = $stdout.sync
$stdout.reopen(saved)
p [on_file, $stdout.sync]
