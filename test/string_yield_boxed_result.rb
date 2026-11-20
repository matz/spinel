def collect_boxed_yield(flag)
  values = [:marker]
  values << yield(flag)
  values.last
end

word = +"word"
[false, true].each do |flag|
  result = collect_boxed_yield(flag) do |keep|
    next word if keep
    nil
  end
  p result.nil?, result.equal?(word)
end

kept = collect_boxed_yield(true) do
  begin
    next +"only in the result"
  ensure
    GC.start
    p (+"allocated in ensure").size
  end
end
p kept << "!"

low = +"a"
high = +"z"
range = low..high
endpoint = collect_boxed_yield(true) { range.begin }
endpoint << "!"
p endpoint.equal?(low), range.begin
