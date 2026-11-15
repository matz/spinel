def inner(n, &callback)
  callback.call(n) { n + 10 }
end
def outer(n, &callback)
  callback.call(n) do
    inner(n + 1) { |value, &blk| value + blk.call }
  end
end
outer(1) { |value, &unused| p value }
