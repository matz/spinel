# Process.spawn with :close as a redirection: the child's descriptor is
# closed, where nil or false leave it inherited. The child asks the shell to
# dup the descriptor, which fails on a closed one in every sh alike, and says
# so on the descriptor named.
$stdout.sync = true
# (the message of a failed dup goes to a stderr of its own for 0 and 1; a probe
# of 2 lets it go to 2, which is closed when the dup fails, or it would open 2)
probe = ->(fd, to) { "(exec 3<&#{fd}) #{fd == 2 ? "" : "2>/dev/null "}&& echo '#{fd}: open' >&#{to} || echo '#{fd}: closed' >&#{to}" }
def capture
  r, w = IO.pipe
  yield w
  w.close
  print r.read
  r.close
end
capture { |w| Process.wait(spawn('sh', '-c', "#{probe[0, 1]}; #{probe[2, 1]}", in: :close, out: w)) }
capture { |w| Process.wait(spawn('sh', '-c', "#{probe[0, 2]}; #{probe[1, 2]}", out: :close, err: w)) }
capture { |w| Process.wait(spawn('sh', '-c', "#{probe[1, 1]}; #{probe[2, 1]}", err: :close, out: w)) }
capture { |w| system('sh', '-c', probe[0, 1], in: :close, out: w) }
p $?.success?
capture { |w| Process.wait(spawn('sh', '-c', "#{probe[1, 3]}; #{probe[2, 3]}", [:out, :err] => :close, 3 => w)) }
# the copies come before the close: the child's stderr is the parent's
# stdout, and its stdout is closed
Process.wait(spawn('sh', '-c', "echo 'err: the parent stdout' >&2; #{probe[1, 2]}", out: :close, err: 1))
