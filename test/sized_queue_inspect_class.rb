q = SizedQueue.new(1)
p q.inspect.start_with?("#<Thread::SizedQueue:")
p q.to_s.start_with?("#<Thread::SizedQueue:")
p Thread::SizedQueue.new(2).inspect.start_with?("#<Thread::SizedQueue:")
p Queue.new.inspect.start_with?("#<Thread::Queue:")
q << 1
p q.inspect.start_with?("#<Thread::SizedQueue:")
p q.pop
q.close
p q.inspect.start_with?("#<Thread::SizedQueue:")
def new_queue
  print "N"
  SizedQueue.new(1)
end
p new_queue.inspect.start_with?("#<Thread::SizedQueue:")
