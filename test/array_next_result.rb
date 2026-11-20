trace = []
p Array.new(3) { |i| begin; next i if i == 0; i + 10; ensure; trace << i; GC.start; end }
p trace

p Array.new(2) { |i|
  begin
    next Array.new(2) { |j|
      begin
        next j if j == 0
        j + 2
      ensure
        GC.start
      end
    } if i == 0
    [7, 8]
  ensure
    GC.start
  end
}

word = +"word"
trace = []
begin
  p Array.new(2) { |i|
    begin
      begin
        next word if i == 0
        word
      ensure
        trace << :inner
        GC.start
      end
    ensure
      trace << :outer
      GC.start
    end
  }
  trace << :after
ensure
  trace << :finished
end
p trace
