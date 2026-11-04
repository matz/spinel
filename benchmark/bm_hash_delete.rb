# Delete/reinsert mixed values through fresh equal String keys, as a prepared
# statement LRU does. A mutable lookup key cannot reuse a cached String hash.
cache = { "select id from users where id = ?" => 7, "other" => "value" }
total = 0
8_000_000.times do
  key = +"select id from users where id = ?"
  value = cache.delete(key)
  total += value
  cache[key] = value
end
p total
