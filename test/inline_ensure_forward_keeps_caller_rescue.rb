# A return through two ensures of a yielding method spliced into a rescue
# body leaves the caller's handler in place: the inner ensure hands the
# return to the outer one and pops only the rescue bodies opened since the
# outer ensure began, so $! is still the rescued exception after the call.
def nest(xs)
  begin
    begin
      xs.each { |x| return yield(x) if x > 1 }
    ensure
      $log << :inner
    end
  ensure
    $log << :outer
  end
  nil
end
$log = []

def run
  begin
    raise "boom"
  rescue
    v = nest([1, 2, 3]) { |x| x * 10 }
    p [v, $!&.message, $log]
    begin
      raise ArgumentError, "two"
    rescue ArgumentError => e
      p e.message
    end
    p $!&.message
  end
  p $!
  begin
    raise "three"
  rescue => e3
    p e3.message
  end
end
run

# the same nesting in a method of its own, inside its rescue body
def own(xs)
  begin
    raise "own"
  rescue
    begin
      begin
        xs.each { |x| return [x, $!.message] if x > 1 }
      ensure
        $log << :own_inner
      end
    ensure
      $log << :own_outer
    end
  end
  nil
end
p own([1, 5])
p $log
p $!
