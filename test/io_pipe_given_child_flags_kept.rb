# A pipe's write end given to a child before the parent writes to it keeps
# the blocking mode the child takes it in: the parent's write afterwards, or
# a dup's, must not make the child's writes fail with EAGAIN and lose bytes.
# The test runs itself as the child, which waits for the parent's writes,
# with a reader that starts late.
if ENV["SP_PIPE_CHILD"]
  $stdin.read   # held until the parent's writes are done
  s = "x" * 1023
  2000.times { puts s }
  exit 0
end

cmd = RUBY_ENGINE == "spinel" ? [$0] : [ENV["RUBY"] || "ruby", $0]
r, w = IO.pipe
gate_r, gate_w = IO.pipe
pid = Process.spawn({ "SP_PIPE_CHILD" => "1" }, *cmd, out: w, in: gate_r)
gate_r.close
w.write("parent\n")
d = w.dup
d.write("dup\n")
d.close
w.close
gate_w.close   # the child starts writing only now, into the full-size pipe
sleep 0.5
n = r.read.bytesize
Process.wait(pid)
p [n, $?.success?]
