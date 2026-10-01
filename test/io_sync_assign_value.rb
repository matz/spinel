# `io.sync = v` -- and `io.public_send(:sync=, v)` -- answers v, whatever v
# is; only its truth sets the mode. It answered the Boolean it set, so a
# send whose arms answer any value (activesupport's Object#with sets
# attributes through public_send) had an arm of the wrong C type.
io = $stdout
p(io.sync = 1)
p io.sync
p(io.sync = nil)
p io.sync
p io.public_send(:sync=, "yes")
p io.sync
x = [io, 1][ARGV.size]
p(x.sync = :on)
p(x.sync = false)
p io.sync
