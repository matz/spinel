# IO.pipe at the descriptor limit raises CRuby's Errno::EMFILE, in the
# destructured form (`r, w = IO.pipe`) and the plain one, and a pipe made
# before the limit is still open.
soft, max = Process.getrlimit(:NOFILE)
first_r, first_w = IO.pipe
Process.setrlimit(:NOFILE, 32, max)
held = []
begin
  loop do
    r, w = IO.pipe
    held << r << w
  end
rescue SystemCallError => e
  p e.class
end
begin
  held.concat(IO.pipe)
rescue SystemCallError => e
  p e.class
end
p held.map(&:fileno).uniq.size == held.size, held.none?(&:closed?)
first_w.write("x")
p first_r.read(1)
Process.setrlimit(:NOFILE, soft, max)   # a close wants a descriptor of its own (macOS)
held.each(&:close)
p :done
