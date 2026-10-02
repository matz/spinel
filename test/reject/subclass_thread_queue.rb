# Thread::Queue names the builtin Queue (#7075).
class Jobs < Thread::Queue
end

q = Jobs.new
q << 1
p q.pop
