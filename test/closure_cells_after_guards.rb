# A method that opens with guards and then creates a real closure: the
# captured locals keep their values across the guards, the closure and
# the statements around it.

class Bag
  def initialize(items)
    @items = items
  end

  def each
    @items.each { |x| yield x }
  end
end

class Route
  def initialize(segments)
    @segments = segments
  end

  # a guard that reads the captured parameter, a local captured by the block
  def match(segs)
    return nil if segs.empty?
    return :short unless segs.size > 0 && segs.size >= 1
    captured = {}
    @segments.each do |seg|
      captured[seg] = segs[0]
    end
    captured
  end

  # a captured parameter written after the guards, before and after the block
  def tally(n, step)
    return -1 if n < 0
    return 0 if n == 0
    total = 0
    n += 1
    @segments.each { |s| total += n; n += step }
    n += 100
    [total, n]
  end

  # a block created in a branch, and a block created in a loop
  def pick(flag, xs)
    return [] if xs.nil?
    out = []
    if flag
      @segments.each { |s| out << "#{s}#{xs.size}" }
    else
      out << "none"
    end
    i = 0
    while i < 2
      @segments.each { |s| out << "#{s}:#{i}" }
      i += 1
    end
    out
  end

  # nested blocks capturing the same locals, and a proc that outlives the call
  def counter(start)
    return nil if start.nil?
    count = start
    @segments.each do |s|
      @segments.each { |t| count += 1 }
    end
    bump = proc { count += 10 }
    bump.call
    [count, bump]
  end

  # Float, nilable and boxed captures
  def mix(f, k)
    return 0.0 unless f
    acc = f
    last = nil
    @segments.each { |s| acc += 1.5; last = s }
    [acc, last, k]
  end
end

[["a", "b"], Bag.new(["p", "q"])].each do |segs|
  r = Route.new(segs)
  p r.match([])
  p r.match(["y"])
  p r.tally(-3, 1)
  p r.tally(0, 1)
  p r.tally(2, 3)
  p r.pick(true, nil)
  p r.pick(true, [1, 2])
  p r.pick(false, [1])
  p r.counter(nil)
  c, b = r.counter(5)
  p c
  p b.call
  p b.call
  p r.mix(nil, 1)
  p r.mix(2.0, :k)
end

r = Route.new(Bag.new(["z"]))
n = 0
1000.times { n += 1 if r.match([]).nil? }
p n
p r.match(["w"])
