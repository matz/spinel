# NoMethodError.new(msg, name, args, private, receiver: obj): the fourth
# positional argument answers #private_call? beside the receiver: keyword,
# as it does without the keyword.
o = Object.new
e = NoMethodError.new("m", :late, [1], true, receiver: o)
p e.private_call?, e.args, e.name, e.receiver.equal?(o)
f = NoMethodError.new("m", :late, [], false, receiver: o)
p f.private_call?
g = NoMethodError.new("m", :late, [2], receiver: o)
p g.private_call?, g.args
flag = ARGV.size == 0
h = NoMethodError.new("m", :late, [], flag, receiver: 5)
p h.private_call?, h.receiver
i = NoMethodError.new("m", :late, [], true)
p i.private_call?
