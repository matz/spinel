# A Mutex, Queue, SizedQueue or ConditionVariable read back boxed is_a? the
# top-level name too (its class is named Thread::Mutex), and a SizedQueue a Queue.
y = [Mutex.new, 1][ARGV.size]
p y.is_a?(Mutex)
p y.is_a?(Thread::Mutex)
p y.kind_of?(Mutex)
p Mutex === y
p y.instance_of?(Thread::Mutex)
q = [Queue.new, 1][ARGV.size]
p q.is_a?(Queue)
c = [ConditionVariable.new, 1][ARGV.size]
p c.is_a?(ConditionVariable)
s = [SizedQueue.new(1), 1][ARGV.size]
p s.is_a?(Queue)
p s.is_a?(SizedQueue)
p(case y when Mutex then :mutex else :other end)
