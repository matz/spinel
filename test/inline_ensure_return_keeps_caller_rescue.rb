# A yielding method spliced into a rescue body, whose block returns through
# the method's ensure, leaves the caller's handler in place: $! is still the
# rescued exception after the call. A rescue body inside the method is left.
def ens(xs)
  begin
    xs.each { |x| return yield(x) if x > 1 }
  ensure
    $e = 1
  end
  nil
end

def ens_in_rescue(xs)
  begin
    raise "inner"
  rescue
    begin
      xs.each { |x| return yield(x) if x > 1 }
    ensure
      $e = 2
    end
  end
  nil
end

def run
  begin
    raise "boom"
  rescue
    w = ens([1, 2, 3]) { |x| x * 100 }
    p [w, $!&.message, $e]
    v = ens_in_rescue([1, 2, 3]) { |x| x * 10 }
    p [v, $!&.message, $e]
  end
  p $!
  begin
    raise ArgumentError, "two"
  rescue ArgumentError => e
    p e.message
  end
end
run
