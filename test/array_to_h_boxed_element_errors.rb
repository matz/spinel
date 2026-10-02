# to_h on Arrays read out of a container: the same errors as a typed Array's
[[1, 2], [1.5], ["a", "b"], [[1, 2, 3]], [], [[:a, 1]], [[1, :b]]].each do |arr|
  begin
    p arr.to_h
  rescue => e
    p [e.class, e.message]
  end
end
