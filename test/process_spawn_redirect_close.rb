# Process.spawn with :close as a redirection: the child's descriptor is
# closed, where nil or false leave it inherited.
pid = spawn('sh', '-c', 'read x 2>/dev/null; echo "in: $?"', in: :close)
Process.wait(pid)
p $?.exitstatus

r, w = IO.pipe
pid = spawn('sh', '-c', 'echo out 2>/dev/null; echo "out: $?" >&2', out: :close, err: w)
w.close
Process.wait(pid)
puts r.read
r.close

pid = spawn('sh', '-c', 'echo err >&2 2>/dev/null; echo "err: $?"', err: :close)
Process.wait(pid)

system('sh', '-c', 'read x 2>/dev/null; echo "system in: $?"', in: :close)
p $?.exitstatus

# a group
pid = spawn('sh', '-c', 'echo both; echo both >&2; echo "group: $?"', [:out, :err] => :close)
Process.wait(pid)
p $?.exitstatus
