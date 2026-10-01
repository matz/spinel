# An optional block parameter's default that writes an ivar, in a block
# instance_exec runs on a value with no ivar layout: the default runs under
# the receiver, which has no slot to hold it, so it is refused as a write in
# the body is. It wrote the caller's ivar.
@d = 7
p(Object.new.instance_exec { |a = (@d = 1)| [a, @d] })
p @d
