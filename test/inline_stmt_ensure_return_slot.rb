# A yielding method called as a statement, whose block returns through the
# method's ensure or Mutex#synchronize, compiles in a method that answers
# another type: the call has no result slot, so the return's value is only
# evaluated.
M = Mutex.new
def ens(xs)
  begin
    xs.each { |x| return yield(x) if x > 1 }
  ensure
    $e = 1
  end
  nil
end

def sync(xs)
  M.synchronize do
    xs.each { |x| return yield(x) if x > 1 }
  end
  nil
end

def run
  ens([1, 2, 3]) { |x| $seen = x * 100 }
  sync([1, 2, 3]) { |x| $seen2 = x * 10 }
  [1]
end
p run
p [$seen, $seen2, $e]
