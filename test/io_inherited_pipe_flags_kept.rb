# A write that waits for a full pipe leaves the flags of a descriptor the
# program did not make as they were. The test runs itself as a child whose
# stdout and stderr are one pipe, as `2>&1` makes them, with a reader that
# starts late: warn on stderr must not make stdout's writes fail with EAGAIN
# and lose bytes.
if ENV["SP_PIPE_CHILD"]
  warn "start"
  s = "x" * 1023
  2000.times { puts s }
  warn "end"
  exit 0
end

cmd = RUBY_ENGINE == "spinel" ? [$0] : [ENV["RUBY"] || "ruby", $0]
r, w = IO.pipe
pid = Process.spawn({ "SP_PIPE_CHILD" => "1" }, *cmd, out: w, err: w)
w.close
sleep 0.5
n = r.read.bytesize
Process.wait(pid)
p [n, $?.success?]
