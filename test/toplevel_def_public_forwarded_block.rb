# A `...` forwarder hands the block it was given on to a public top-level def
# it calls with an explicit receiver.
$log = []
def recv = ($log << :recv; 3)
def arg = ($log << :arg; 1)
def mk = ($log << :mk; proc { 2 })
def g1(a) = [a, yield]
public :g1
def fwd(...) = 3.g1(...)
p fwd(1, &mk)
$log = []
def fwd2(...) = recv.g1(...)
p fwd2(arg, &mk), $log
