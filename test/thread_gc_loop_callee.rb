# spinel: decisions
# spinel: gc-stress
# Recurse from a loop with a live root frame on each worker. Park, collect,
# and unwind one iteration through the caller's rescue.
class Reader
  def pair_sum(value, interrupt)
    data = [value, value + 1]
    return pair_sum(-value, interrupt) if value < 0
    Thread.pass
    GC.start
    raise "retry" if interrupt
    # The temporary array also needs a root inside the expression.
    data.fetch(0) + [data.fetch(1)].fetch(0)
  end

  def sum(base)
    i = 0
    total = 0
    while i < 8
      begin
        total += pair_sum(-(base + i), i == 4)
      rescue RuntimeError
        total += 1
      end
      i += 1
    end
    total
  end
end

first = Thread.new { Reader.new.sum(10) }
second = Thread.new { Reader.new.sum(100) }
puts first.value
puts second.value
