# Process.spawn with :close as a redirection: the child's descriptor is
# closed. The child reports through its status, which does not depend on the
# shell's wording of a closed stream (dash and bash differ in what they print
# and in the status code); a write to a closed stream is silenced in the
# child, so no shell's complaint reaches this program's streams.
def failed?(cmd, **opts)
  pid = spawn('sh', '-c', cmd, **opts)
  Process.wait(pid)
  !$?.success?
end
p failed?('read x 2>/dev/null', in: :close)
p failed?('read x </dev/null 2>/dev/null')          # the stream open: read reaches its end, status 1 either way
p failed?('echo out 2>/dev/null', out: :close)
p failed?('echo out 2>/dev/null')
p failed?('echo err >&2 2>/dev/null', err: :close)
p failed?('echo err 2>/dev/null >&2')   # the open stream, its line kept off this program's stderr
system('sh', '-c', 'read x 2>/dev/null', in: :close)
p $?.success?
# a group
p failed?('{ echo both; echo both >&2; } 2>/dev/null', [:out, :err] => :close)
# the other streams stay open: err carries a line out while out is closed
r, w = IO.pipe
pid = spawn('sh', '-c', 'echo "err stays open" >&2', out: :close, err: w)
w.close
Process.wait(pid)
puts r.read
r.close
