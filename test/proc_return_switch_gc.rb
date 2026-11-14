# A green thread switched out inside a method that holds a proc-return home,
# then returned from that method and ran on. The copy of its handler state
# taken at the switch still named the home's C frame, and a collection that
# another thread started while this one's worker was parked walked that frame
# after later calls had written over it (SIGSEGV in sp_exc_ctx_mark).
def call_it(blk)
  blk.call
end

def home
  pr = proc do
    Thread.pass
    return 1
  end
  call_it(pr)
  0
end

def work(n)
  a = []
  n.times { |i| a << "s#{i}" }
  a.size
end

threads = Array.new(8) do
  Thread.new do
    s = 0
    3000.times { s += home + work(100) }
    s
  end
end
p threads.sum(&:value)
